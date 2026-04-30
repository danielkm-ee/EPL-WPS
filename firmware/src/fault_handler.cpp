/* fault_handler.cpp */

#include <Arduino.h>
#include "hardware/gpio.h"

#include "fault_handler.h"
#include "pwm_control.h"
#include "telemetry.h"
#include "../config.h"

static volatile bool           pending = false;
static volatile FaultStateType active  = PMM_FAULT_TYPE;

// Entry point used by ISRs and by in-line detectors (e.g. boost voltage-table
// build on a missing module). Immediately forces the output stage OFF so the
// ISR path is safe even before fault_handle() runs.
void fault_trip(FaultStateType type) {
    pwm_disable_output_stage();
    active  = type;
    pending = true;
}

bool fault_pending(void) {
    return pending;
}

FaultStateType fault_current_type(void) {
    return active;
}

const char *fault_type_name(FaultStateType type) {
    switch (type) {
        case PMM_FAULT_TYPE:                 return "PMM_FAULT";
        case BOOST_CONVERTER_PGOOD_FAULT:    return "BOOST_CONVERTER_PGOOD_FAULT";
        case POWER_OUT_OF_RANGE_FAULT:       return "POWER_OUT_OF_RANGE_FAULT";
        case HIGH_VOLTAGE_PHASE_SETUP_FAULT: return "HIGH_VOLTAGE_PHASE_SETUP_FAULT";
    }
    return "UNKNOWN_FAULT";
}

//-----------------------------------------------------------------------------
// ISRs — thin. The heavy lifting runs in fault_handle() on the main loop.
//-----------------------------------------------------------------------------
static void isr_pmm(void) {
    gpio_acknowledge_irq(PMM_FAULT_PIN, GPIO_IRQ_EDGE_FALL);
    fault_trip(PMM_FAULT_TYPE);
}
static void isr_boost(void) {
    gpio_acknowledge_irq(BOOST_PGOOD_PIN, GPIO_IRQ_EDGE_FALL);
    fault_trip(BOOST_CONVERTER_PGOOD_FAULT);
}

void fault_attach_interrupts(void) {
    attachInterrupt(digitalPinToInterrupt(PMM_FAULT_PIN),  isr_pmm,   FALLING);
    attachInterrupt(digitalPinToInterrupt(BOOST_PGOOD_PIN), isr_boost, FALLING);
    // OUTPUT_OVERCURRENT is wired by firmware.ino — it dispatches by mode.
    // The pi-filter HC module has no PGOOD line so there is no HC-PGOOD ISR.
}

//-----------------------------------------------------------------------------
// Main-loop handler. Waits for recovery on faults whose GPIO can clear; for
// the rest, services serial commands until reset.
//-----------------------------------------------------------------------------
static void service_serial_until(bool (*recovered)(void)) {
    gpio_put(STATUS_LED_PIN, false);
    while (recovered == NULL || !recovered()) {
        if (Serial.available()) {
            String command = Serial.readStringUntil('\n');
            telemetry_process_command(command);
        }
    }
    gpio_put(STATUS_LED_PIN, true);
}

static bool boost_recovered(void) { return gpio_get(BOOST_PGOOD_PIN) != 0; }

void fault_handle(void) {
    if (!pending) return;

    pwm_disable_output_stage();
    pwm_set_feedback_duty(0.0);

    Serial.print("FAULT: ");
    Serial.println(fault_type_name(active));

    switch (active) {
        case PMM_FAULT_TYPE:
            // The original behaviour was to block 500 ms then clear. We
            // preserve that so recovery telemetry stays stable.
            gpio_put(STATUS_LED_PIN, false);
            delay(500);
            gpio_put(STATUS_LED_PIN, true);
            Serial.println("PMM Fault Cleared");
            break;
        case BOOST_CONVERTER_PGOOD_FAULT:
            service_serial_until(boost_recovered);
            Serial.println("High Voltage Phase Fault Cleared");
            break;
        case POWER_OUT_OF_RANGE_FAULT:
        case HIGH_VOLTAGE_PHASE_SETUP_FAULT:
            // Non-recoverable: wait for operator to RESET_DEVICE.
            service_serial_until(NULL);
            break;
    }

    pending = false;
}
