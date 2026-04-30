// Rack Robotics, Inc. - Powercore Development Firmware
// For use with the arduino-pico plugin by Earle Philhower.
// Copyright (c) 2025 Rack Robotics, Inc. All rights reserved.
// This firmware is for Powercore hardware revision F & G.
//
// Modified for the EPL WPS system by Daniel Monahan.

#include <Arduino.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "config.h"
#include "src/types.h"
#include "src/sensors.h"
#include "src/pwm_control.h"
#include "src/boost_module.h"
#include "src/fault_handler.h"
#include "src/telemetry.h"

//=============================================================================
// Device-level runtime state (read by telemetry.cpp via extern)
//=============================================================================
volatile DeviceState     deviceState             = STARTUP;
volatile ModeOfOperation modeOfOperation         = EDM_ISOFREQUENCY_MODE;
volatile bool            modePreparationComplete = false;

// Machining parameters (mutated by telemetry.cpp on SET_ALL_PARAMETERS).
volatile int    dischargeCountTarget = 0;
volatile double machiningDutyCycle   = 0.10;
volatile int    machiningFrequency   = 10000;
volatile int    machiningInitVoltage = 80;

// EDM iso-frequency mode runtime.
volatile bool   newDischargeDetected            = false;
volatile int    newDischargeCurrent_ADC         = 0;
volatile int    newDischargeVoltage_ADC         = 0;
volatile double avgDischargeCurrent             = 0.0;
volatile double avgDischargeVoltage             = 0.0;
volatile double avgDischargeSuccessRate         = 0.0;
volatile double dischargeSuccessRate            = 0.0;
volatile int    dischargeRateCalculationInterval_MICROS = 0;
volatile int    currentDischargeCount           = 0;
volatile int    dischargesSinceOperationStarted = 0;
volatile bool   requestedDischargesReached      = false;
volatile unsigned long lastDischargeSuccessRateCalculationTime = 0;

// Edge-detection mode runtime.
volatile bool edgeDetected = false;

// Boost-converter dpot handle and calibration table.
boost_dpot_t*    g_boost_dpot = nullptr;
boost_cal_table_t g_boost_cal;

// Periodic timing.
static unsigned long lastTelemetryEventTime_MS            = 0;
static unsigned long lastPeripheralManagementEventTime_MS = 0;

//=============================================================================
// Forward declarations
//=============================================================================
static void handle_peripheral_management(void);
static void update_device_statistics(void);
static void handle_operating_state(void);
static void handle_idle_state(void);

static void edm_isofreq_mode(void);
static void edge_detection_mode(void);
static void setup_iso_output_pwm(double duty, int frequency_hz);
static void setup_edge_output_pwm(void);

static void output_overcurrent_isr(void);
static void output_overcurrent_isr_iso(void);
static void output_overcurrent_isr_edge(void);

//=============================================================================
// Setup helpers
//=============================================================================
// PWM pin functions (SW_*, OUTPUT_OVERCURRENT_SET, EDM_FEEDBACK) are set by
// pwm_setup_feedback() and pwm_setup_output_stage() — no need to set them here.
static void setup_pin_modes(void) {
    pinMode(EDM_ENABLE,         INPUT);
    pinMode(OUTPUT_OVERCURRENT, INPUT);
    pinMode(BOOST_PGOOD,        INPUT);
    pinMode(PMM_FAULT,          INPUT);
    pinMode(PMM_DIAG_EN,        OUTPUT);
    pinMode(PMM_ENABLE,         OUTPUT);
    pinMode(STATUS_LED,         OUTPUT);

    pinMode(PMM_ISENSE,    INPUT);
    pinMode(OUTPUT_VSENSE, INPUT);
    pinMode(OUTPUT_ISENSE, INPUT);
}

static void setup_initial_pin_states(void) {
    gpio_put(PMM_ENABLE,  false);
    gpio_put(PMM_DIAG_EN, true);
}

// PMM high-side switch on; spin until inrush settles.
static void enable_pmm_and_wait_inrush(void) {
    gpio_put(PMM_ENABLE, true);
    unsigned long start = millis();
    while (millis() - start < PMM_INRUSH_DELAY_MS) {
        // Inrush settling.
    }
}

//=============================================================================
// Arduino entry points
//=============================================================================
void setup(void) {
    deviceState = STARTUP;

    setup_pin_modes();
    setup_initial_pin_states();
    sensors_setup_adc();
    pwm_setup_feedback();
    pwm_disable_output_stage();

    // Calibrate PMM zero-current with the high-side switch still off.
    sensors_calibrate_input_current();
    enable_pmm_and_wait_inrush();
    sensors_calibrate_output_current();

    telemetry_setup_serial();
    Serial.print("Software Version: ");
    Serial.println(SOFTWARE_VERSION);

    fault_attach_interrupts();
    attachInterrupt(digitalPinToInterrupt(OUTPUT_OVERCURRENT),
                    output_overcurrent_isr, FALLING);

    // setup output stage to hold HVP voltage level
    pwm_setup_output_stage(0.00f, 10000, DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                           false, false, true,
                           EDM_ISOFREQ_HV_PULSE_ON_TIME_US,
                           EDM_ISOFREQ_HV_PWM_OFFSET);
    // build boost converter voltage table
    g_boost_dpot = boost_dpot_create(I2C_PORT, DPOT_ADDR, DPOT_REG);
    boost_setup_i2c(g_boost_dpot, I2C_SDA_PIN, I2C_SCL_PIN, I2C_BAUD_RATE_HZ);
    boost_cal_build(g_boost_dpot, &g_boost_cal, 8, 20);
    // disable output stage until machining mode occurs
    pwm_disable_output_stage()

    gpio_put(STATUS_LED, true);
    Serial.println("Setup complete");
    deviceState = IDLE;
}

void loop(void) {
    unsigned long now_ms = millis();

    if (fault_pending()) {
        deviceState = FAULT;
        fault_handle();
        deviceState = IDLE;
    }

    if (now_ms - lastPeripheralManagementEventTime_MS >= peripheralManagementInterval_MS) {
        handle_peripheral_management();
        lastPeripheralManagementEventTime_MS = now_ms;
    }

    switch (deviceState) {
        case OPERATING: handle_operating_state(); break;
        case IDLE:      handle_idle_state();      break;
        default:        break;
    }

    if (sendPeriodicTelemetryEnabled
        && now_ms - lastTelemetryEventTime_MS >= periodicTelemetryInterval_MS) {
        telemetry_send();
        lastTelemetryEventTime_MS = now_ms;
    }
}

//=============================================================================
// Peripheral management — runs every peripheralManagementInterval_MS
//=============================================================================
static void handle_peripheral_management(void) {
    sensors_sample_input_current();
    update_device_statistics();

    deviceState = digitalRead(EDM_ENABLE) ? OPERATING : IDLE;

    if (deviceState == OPERATING) {
        float power_w = sensors_avg_input_current() * INPUT_SUPPLY_VOLTAGE;
        float ratio   = power_w / MAX_INPUT_POWER_SETPOINT_WATTS;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        // Active-low feedback duty: full duty = no power.
        pwm_set_feedback_duty(1.0 - ratio);
    } else {
        pwm_set_feedback_duty(0.0);
    }

    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        telemetry_process_command(command);
    }
}

static void update_device_statistics(void) {
    if (sensors_avg_input_current() > MAX_SAFE_INPUT_CURRENT) {
        fault_trip(POWER_OUT_OF_RANGE_FAULT);
    }
}

static void handle_operating_state(void) {
    switch (modeOfOperation) {
        case EDM_ISOFREQUENCY_MODE: edm_isofreq_mode();    break;
        case EDGE_DETECTION_MODE:   edge_detection_mode(); break;
    }
}

static void handle_idle_state(void) {
    pwm_disable_output_stage();
    modePreparationComplete         = false;
    dischargesSinceOperationStarted = 0;
    delay(100);
}

//=============================================================================
// Output overcurrent ISR — dispatches by mode
//=============================================================================
static void output_overcurrent_isr(void) {
    switch (modeOfOperation) {
        case EDM_ISOFREQUENCY_MODE: output_overcurrent_isr_iso();  break;
        case EDGE_DETECTION_MODE:   output_overcurrent_isr_edge(); break;
        default:
            pwm_disable_output_stage();
            Serial.println("OUTPUT OVERCURRENT HANDLER ERROR");
            break;
    }
}

//=============================================================================
// EDM Iso-frequency mode — constant-frequency discharge generator
//=============================================================================
static void edm_isofreq_mode(void) {
    if (newDischargeDetected) {
        newDischargeDetected = false;
        double i = sensors_adc_to_discharge_current(newDischargeCurrent_ADC);
        double v = sensors_adc_to_discharge_voltage(newDischargeVoltage_ADC);
        avgDischargeCurrent = (0.90 * avgDischargeCurrent) + (0.10 * i);
        avgDischargeVoltage = (0.90 * avgDischargeVoltage) + (0.10 * v);
    }

    bool interval_elapsed =
        (micros() - lastDischargeSuccessRateCalculationTime
         > (uint32_t)dischargeRateCalculationInterval_MICROS);

    if (interval_elapsed || currentDischargeCount >= DISCHARGES_PER_CALC_INTERVAL) {
        dischargeSuccessRate    = (double)currentDischargeCount
                                  / (double)DISCHARGES_PER_CALC_INTERVAL;
        avgDischargeSuccessRate = (0.99 * avgDischargeSuccessRate)
                                  + (0.01 * dischargeSuccessRate);

        if (dischargeSuccessRate >= MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD
            && allowDischargeSuccessRatePulseSkipping) {
            pwm_disable_output_stage();
            Serial.println("Pulse skipping: discharge success rate threshold exceeded");
            unsigned long t0 = millis();
            while (millis() - t0 < 250) {}
            dischargeSuccessRate = 0;
            Serial.println("Pulse skipping complete");
        }
        if ((sensors_avg_input_current() * INPUT_SUPPLY_VOLTAGE)
              > MAX_INPUT_POWER_SETPOINT_WATTS
            && allowPowerSetpointPulseSkipping) {
            pwm_disable_output_stage();
            Serial.println("Pulse skipping: power setpoint exceeded");
            unsigned long t0 = millis();
            while (millis() - t0 < 10) {}
            Serial.println("Pulse skipping complete");
        }

        currentDischargeCount = 0;
        lastDischargeSuccessRateCalculationTime = micros();
    }

    if (dischargesSinceOperationStarted >= dischargeCountTarget && dischargeCountTarget != 0) {
        if (!requestedDischargesReached) {
            Serial.print("Number of discharges requested reached: ");
            Serial.println(dischargesSinceOperationStarted);
            pwm_disable_output_stage();
        }
        pwm_set_feedback_duty(0.0);
        requestedDischargesReached = true;
        return;
    }

    requestedDischargesReached = false;
    if (!modePreparationComplete) {
        setup_iso_output_pwm(machiningDutyCycle, machiningFrequency);
        modePreparationComplete = true;
    }
}

static void output_overcurrent_isr_iso(void) {
    newDischargeCurrent_ADC = analogRead(OUTPUT_ISENSE);
    newDischargeVoltage_ADC = analogRead(OUTPUT_VSENSE);
    dischargesSinceOperationStarted++;
    currentDischargeCount++;
    newDischargeDetected = true;
    gpio_acknowledge_irq(OUTPUT_OVERCURRENT, GPIO_IRQ_EDGE_FALL);
}

static void setup_iso_output_pwm(double duty, int frequency_hz) {
    pwm_disable_output_stage();
    dischargeRateCalculationInterval_MICROS =
        (int)(DISCHARGES_PER_CALC_INTERVAL * (1000000 / frequency_hz));
    boost_set_voltage(g_boost_dpot, &g_boost_cal, machiningInitVoltage, BOOST_CONVERTER_RAMP_SPACING_MS);
    pwm_setup_output_stage(duty, frequency_hz, DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                           true, true, true,
                           EDM_ISOFREQ_HV_PULSE_ON_TIME_US,
                           EDM_ISOFREQ_HV_PWM_OFFSET);
}

//=============================================================================
// Edge detection mode — single-pulse workpiece edge detection
//=============================================================================
static void edge_detection_mode(void) {
    if (!modePreparationComplete) {
        pwm_set_feedback_duty(0.0);
        setup_edge_output_pwm();
        modePreparationComplete = true;
    }
}

static void output_overcurrent_isr_edge(void) {
    if (edgeDetected) return;
    edgeDetected = true;
    gpio_acknowledge_irq(OUTPUT_OVERCURRENT, GPIO_IRQ_EDGE_FALL);
    pwm_disable_output_stage();
    pwm_set_feedback_duty(1.0);
    Serial.println("EDGE DETECTED");
    delay(1000);
    pwm_set_feedback_duty(0.0);
    delay(1000);
    edgeDetected = false;
}

static void setup_edge_output_pwm(void) {
    boost_set_voltage(g_boost_dpot, &g_boost_cal, MIN_HIGH_VOLTAGE_PHASE_VOLTS, BOOST_CONVERTER_RAMP_SPACING_MS);
    pwm_setup_output_stage(0.05, 10000, DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                           true, true, false,
                           1, 0.25);
}
