/* boost_module.cpp */

#include <Arduino.h>
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "boost_module.h"
#include "pwm_control.h"
#include "sensors.h"
#include "fault_handler.h"
#include "../config.h"

// Lookup table: dpot_voltage[i] is the measured output voltage (V) when
// the DPOT sits at position i. Built at startup by boost_build_voltage_table.
static int dpot_voltage[MAX_BOOST_CONVERTER_DPOT_POSITION + 1];

void boost_setup_i2c(void) {
    i2c_init(I2C_PORT, I2C_BAUD_RATE_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
}

uint8_t boost_dpot_read(void) {
    uint8_t value = 0;
    uint8_t reg   = DPOT_REG;
    i2c_write_blocking(I2C_PORT, DPOT_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, DPOT_ADDR, &value, 1, false);
    return value;
}

void boost_dpot_write(int position) {
    if (position < MIN_BOOST_CONVERTER_DPOT_POSITION) position = MIN_BOOST_CONVERTER_DPOT_POSITION;
    if (position > MAX_BOOST_CONVERTER_DPOT_POSITION) position = MAX_BOOST_CONVERTER_DPOT_POSITION;

    uint8_t data[2] = { DPOT_REG, (uint8_t)position };
    i2c_write_blocking(I2C_PORT, DPOT_ADDR, data, 2, false);
}

void boost_build_voltage_table(void) {
    Serial.println("Updating boost converter DPOT voltage table...");
    delay(500);

    // HV phase enabled but no pulsing so we read a quiet DC rail.
    pwm_setup_output_stage(0.0, 10000, 8, false, false, true,
                           EDM_ISOFREQ_HV_PULSE_ON_TIME_US,
                           EDM_ISOFREQ_HV_PWM_OFFSET);

    // Seed position 64 and verify the DPOT is actually listening.
    boost_dpot_write(64);
    if (boost_dpot_read() != 64) {
        Serial.println("ERROR: DPOT is not at expected position");
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
        return;
    }

    // Ramp down to the minimum before sweeping up.
    for (int i = 64; i >= MIN_BOOST_CONVERTER_DPOT_POSITION; i--) {
        boost_dpot_write(i);
        delay(BOOST_CONVERTER_RAMP_SPACING_MS);
    }

    Serial.println("Waiting for voltage to stabilize...");
    delay(250);

    Serial.println("Reading voltages for DPOT lookup table...");
    for (int i = 0; i <= MAX_BOOST_CONVERTER_DPOT_POSITION; i++) {
        boost_dpot_write(i);
        delay(BOOST_CONVERTER_RAMP_SPACING_MS);
        dpot_voltage[i] = sensors_read_output_voltage_averaged(10);
    }

    Serial.println("Boost Converter Digital Potentiometer Voltage Table:");
    int max_voltage = 0;
    for (int i = 0; i <= MAX_BOOST_CONVERTER_DPOT_POSITION; i++) {
        Serial.print(i); Serial.print(": "); Serial.println(dpot_voltage[i]);
        if (dpot_voltage[i] > max_voltage) max_voltage = dpot_voltage[i];
    }

    // If we can't reach the max safe operating voltage, the module is
    // faulty or unpopulated.
    if (max_voltage < MAX_HIGH_VOLTAGE_PHASE_VOLTS) {
        Serial.println("ERROR: DPOT Voltage range is less than expected range");
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
        return;
    }

    boost_dpot_write(DEFAULT_BOOST_CONVERTER_DPOT_POSITION);
    pwm_disable_output_stage();
}

void boost_set_voltage(int target_volts, int spacing_ms) {
    int current_pos = boost_dpot_read();
    int target_pos  = 0;

    int best_error = 1000;
    for (int i = 0; i <= MAX_BOOST_CONVERTER_DPOT_POSITION; i++) {
        int err = abs(dpot_voltage[i] - target_volts);
        if (err < best_error) {
            best_error = err;
            target_pos = i;
        }
    }

    // 5 V tolerance: beyond that we assume a hardware problem. Retreat to
    // the minimum and trip a fault so the operator notices.
    if (best_error > 5) {
        boost_dpot_write(MIN_BOOST_CONVERTER_DPOT_POSITION);
        Serial.println("ERROR: Difference between target voltage and DPOT voltage is greater than 5 volts");
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
        return;
    }

    if (current_pos == target_pos) {
        boost_dpot_write(target_pos);
        return;
    }
    int step = (current_pos < target_pos) ? 1 : -1;
    for (int i = current_pos; i != target_pos + step; i += step) {
        boost_dpot_write(i);
        delay(spacing_ms);
    }
}
