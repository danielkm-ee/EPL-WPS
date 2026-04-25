/* telemetry.cpp */

#include <Arduino.h>

#include "telemetry.h"
#include "types.h"
#include "sensors.h"
#include "pwm_control.h"
#include "boost_module.h"
#include "fault_handler.h"
#include "../config.h"

const String SOFTWARE_VERSION = "1.0-beta";

//-----------------------------------------------------------------------------
// Externs: device state owned by firmware.ino. Telemetry reads these for the
// status block and writes a small subset (parameters, mode, prep-flag reset)
// in response to serial commands.
//-----------------------------------------------------------------------------
extern volatile DeviceState     deviceState;
extern volatile ModeOfOperation modeOfOperation;
extern volatile bool            modePreparationComplete;
extern volatile bool            edgeDetected;

extern volatile int    dischargeCountTarget;
extern volatile double machiningDutyCycle;
extern volatile int    machiningFrequency;
extern volatile int    machiningInitVoltage;

extern volatile double avgDischargeCurrent;
extern volatile double avgDischargeVoltage;
extern volatile double avgDischargeSuccessRate;
extern volatile int    dischargesSinceOperationStarted;

//-----------------------------------------------------------------------------
// Serial setup
//-----------------------------------------------------------------------------
void telemetry_setup_serial(void) {
    Serial.begin(115200);
    unsigned long start = millis();
    while (millis() - start < 1000) {
        // Give host USB-CDC a chance to enumerate.
    }
}

//-----------------------------------------------------------------------------
// Outbound status block
//-----------------------------------------------------------------------------
static void print_state(DeviceState s) {
    switch (s) {
        case STARTUP:   Serial.println("STARTUP"); break;
        case FAULT:     Serial.println("FAULT"); break;
        case OPERATING: Serial.print("OPERATING: "); break;
        case IDLE:      Serial.println("IDLE"); break;
        default:        Serial.println("UNKNOWN"); break;
    }
}

void telemetry_send(void) {
    Serial.print("FIRMWARE_VERSION ");
    Serial.println(SOFTWARE_VERSION);

    Serial.print("STATE ");
    print_state(deviceState);

    Serial.print("FAULT ");
    if (deviceState == FAULT) {
        Serial.println(fault_type_name(fault_current_type()));
    } else {
        Serial.println("NONE");
    }

    float avg_in_a = sensors_avg_input_current();
    Serial.print("INPUT_CURRENT ");
    Serial.print(avg_in_a, 2);
    Serial.println(" A");
    Serial.print("INPUT_POWER ");
    Serial.print((int)(avg_in_a * INPUT_SUPPLY_VOLTAGE));
    Serial.println("W ");

    if (modeOfOperation == EDGE_DETECTION_MODE) {
        Serial.println(edgeDetected ? "EDGE DETECTED" : "NO EDGE DETECTED YET");
        return;
    }

    // EDM_ISOFREQUENCY_MODE
    Serial.print("AVG_DISCHARGE_CURRENT:");
    Serial.print(avgDischargeCurrent, 2);
    Serial.println(" A");
    Serial.print("AVG_DISCHARGE_VOLTAGE:");
    Serial.print(avgDischargeVoltage, 2);
    Serial.println(" V");
    Serial.print("AVG_OUTPUT_POWER:");
    Serial.print((int)(avgDischargeVoltage * avgDischargeCurrent
                       * machiningDutyCycle * avgDischargeSuccessRate));
    Serial.println(" W");
    Serial.print("DISCHARGES_SINCE_OPERATION_STARTED:");
    Serial.println(dischargesSinceOperationStarted);
    Serial.print("AVG_DISCHARGE_SUCCESS_RATE:");
    Serial.print((int)(avgDischargeSuccessRate * 100));
    Serial.println("%");
    Serial.print("DISCHARGE_SUCCESS_RATE_THRESHOLD:");
    Serial.print((int)(MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD * 100));
    Serial.println("%");
    Serial.println("ISOPULSE PARAMETERS: ");
    if (dischargeCountTarget == 0) {
        Serial.println("Infinite Discharges Requested");
    } else {
        Serial.print("Discharges Requested:");
        Serial.println(dischargeCountTarget);
    }
    Serial.print("Machining Duty Cycle:");
    Serial.println(machiningDutyCycle, 3);
    Serial.print("Machining Frequency:");
    Serial.print(machiningFrequency, 1);
    Serial.println(" Hz");
    Serial.print("Initiation Voltage:");
    Serial.print(machiningInitVoltage, 1);
    Serial.println(" V");
    Serial.println("--------------------------------");
}

//-----------------------------------------------------------------------------
// Parameter validation
//-----------------------------------------------------------------------------
static bool apply_parameters(int discharges, float duty, float frequency, float init_v) {
    if (discharges < 0) {
        Serial.println("ERROR: Invalid discharges value");
        return false;
    }
    if (duty < MIN_DUTY_CYCLE || duty > MAX_DUTY_CYCLE) {
        Serial.print("ERROR: Invalid duty cycle (must be between ");
        Serial.print(MIN_DUTY_CYCLE);
        Serial.print(" and ");
        Serial.print(MAX_DUTY_CYCLE);
        Serial.println(")");
        return false;
    }
    if (frequency < MIN_MACHINING_FREQUENCY_HZ || frequency > MAX_MACHINING_FREQUENCY_HZ) {
        Serial.print("ERROR: Invalid frequency (must be between ");
        Serial.print(MIN_MACHINING_FREQUENCY_HZ);
        Serial.print(" and ");
        Serial.print(MAX_MACHINING_FREQUENCY_HZ);
        Serial.println(" Hz)");
        return false;
    }
    if (init_v < MIN_INIT_VOLTAGE || init_v > MAX_INIT_VOLTAGE) {
        Serial.print("ERROR: Invalid initiation voltage (must be between ");
        Serial.print(MIN_INIT_VOLTAGE);
        Serial.print(" and ");
        Serial.print(MAX_INIT_VOLTAGE);
        Serial.println(" V)");
        return false;
    }

    float period_us  = 1000000.0f / frequency;
    float on_time_us = period_us * duty;
    float off_time_us = period_us - on_time_us;
    if (on_time_us < MIN_ON_TIME_MICROS || on_time_us > MAX_ON_TIME_MICROS) {
        Serial.print("ERROR: Calculated on-time (");
        Serial.print(on_time_us);
        Serial.println(" us) is outside valid range");
        return false;
    }
    if (off_time_us < MIN_OFF_TIME_MICROS) {
        Serial.print("ERROR: Calculated off-time (");
        Serial.print(off_time_us);
        Serial.println(" us) is too short");
        return false;
    }

    dischargeCountTarget    = discharges;
    machiningDutyCycle      = duty;
    machiningFrequency      = (int)frequency;
    machiningInitVoltage    = (int)init_v;
    modePreparationComplete = false;
    return true;
}

//-----------------------------------------------------------------------------
// Command parsing
//-----------------------------------------------------------------------------
static void print_help(void) {
    Serial.println("ERROR: Unknown command");
    Serial.println("Accepted commands:");
    Serial.println("  SEND_TELEMETRY - Send current device status and parameters over serial");
    Serial.println("  SET_ALL_PARAMETERS <discharges> <dutyCycle> <frequency> <initVoltage> - Set all isopulse parameters at once");
    Serial.println("  EDGE_DETECTION_MODE - Enter edge detection mode");
    Serial.println("  EDM_ISOFREQUENCY_MODE - Enter EDM Isofrequency Mode");
    Serial.println("  RESET_DEVICE - Reset the device");
}

void telemetry_process_command(String command) {
    command.trim();

    if (command == "SEND_TELEMETRY") {
        telemetry_send();
        return;
    }
    if (command.startsWith("SET_ALL_PARAMETERS")) {
        String tokens[MAX_PARAMETERS + 1];
        int    count = 0;
        int    pos   = 0;
        while (count < MAX_PARAMETERS + 1 && pos < (int)command.length()) {
            int next = command.indexOf(' ', pos);
            if (next == -1) {
                tokens[count++] = command.substring(pos);
                break;
            }
            tokens[count++] = command.substring(pos, next);
            pos = next + 1;
        }
        if (count != MAX_PARAMETERS + 1) {
            Serial.println("ERROR: Invalid number of parameters");
            Serial.println("Expected 4 parameters: <discharges> <dutyCycle> <frequency> <initVoltage>");
            return;
        }
        OutputParameters p;
        p.dischargeCountTarget = tokens[1].toInt();
        p.dutyCycle            = tokens[2].toFloat();
        p.frequency            = tokens[3].toFloat();
        p.initVoltage          = tokens[4].toFloat();
        if (apply_parameters(p.dischargeCountTarget, p.dutyCycle, p.frequency, p.initVoltage)) {
            Serial.println("OK: Parameters set");
        }
        return;
    }
    if (command.startsWith("EDGE_DETECTION_MODE")) {
        pwm_disable_output_stage();
        modeOfOperation         = EDGE_DETECTION_MODE;
        modePreparationComplete = false;
        return;
    }
    if (command == "EDM_ISOFREQUENCY_MODE") {
        pwm_disable_output_stage();
        modeOfOperation         = EDM_ISOFREQUENCY_MODE;
        modePreparationComplete = false;
        Serial.println("EDM_ISOFREQUENCY_MODE entered");
        return;
    }
    if (command == "RESET_DEVICE") {
        pwm_disable_output_stage();
        Serial.println("Rebooting device");
        rp2040.reboot();
        return;
    }
    if (command.startsWith("SET_DPOT ")) {
        int p = command.substring(9).toInt();
        boost_dpot_write(p);
        Serial.print("DPOT set to: ");
        Serial.println(p);
        return;
    }
    if (command == "READ_HVP_VOLTAGE") {
        int v = sensors_read_output_voltage_averaged(10);
        Serial.print("HVP_AVG_VOLTAGE ");
        Serial.println(v);
        return;
    }
    if (command == "UPDATE_DPOT_VOLTAGE_TABLE") {
        boost_build_voltage_table();
        return;
    }
    if (command.startsWith("SET_DPOT_FROM_VTABLE ")) {
        int target = command.substring(20).toInt();
        if (target < MIN_HIGH_VOLTAGE_PHASE_VOLTS || target > MAX_HIGH_VOLTAGE_PHASE_VOLTS) {
            Serial.print("ERROR: Target voltage out of range (");
            Serial.print(MIN_HIGH_VOLTAGE_PHASE_VOLTS);
            Serial.print("-");
            Serial.print(MAX_HIGH_VOLTAGE_PHASE_VOLTS);
            Serial.println(" V)");
        } else {
            boost_set_voltage(target, BOOST_CONVERTER_RAMP_SPACING_MS);
        }
        return;
    }
    if (command.startsWith("SET_FEEDBACK_DUTY ")) {
        float d = command.substring(18).toFloat();
        if (d < 0.0f || d > 1.0f) {
            Serial.println("ERROR: Duty cycle must be between 0.0 and 1.0");
        } else {
            pwm_set_feedback_duty(d);
            delay(2000);
            Serial.print("EDM Feedback Duty Cycle set to: ");
            Serial.println(d, 3);
        }
        return;
    }

    print_help();
}
