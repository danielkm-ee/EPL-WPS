/* types.h
 *
 * Shared enumerations and structs for the WPS firmware.
 */

#ifndef EPL_WPS_TYPES_H
#define EPL_WPS_TYPES_H

#include <stdint.h>

// Device states.
enum DeviceState {
    STARTUP,
    FAULT,
    OPERATING,
    IDLE
};

// Operating-mode selection.
enum ModeOfOperation {
    EDM_ISOFREQUENCY_MODE,    // Constant-frequency machining discharges
    EDGE_DETECTION_MODE       // Single-pulse workpiece edge detection
};

// Fault classifications.
enum FaultStateType {
    PMM_FAULT_TYPE,
    BOOST_CONVERTER_PGOOD_FAULT,
    POWER_OUT_OF_RANGE_FAULT,
    HIGH_VOLTAGE_PHASE_SETUP_FAULT
};

// Parameter payload for SET_ALL_PARAMETERS.
struct OutputParameters {
    int   dischargeCountTarget;
    float dutyCycle;
    float frequency;
    float initVoltage;
};

// PWM output descriptor: slice/channel pair from the RP2040 PWM peripheral.
struct PWMOutput {
    uint32_t slice;
    uint32_t channel;
};

#endif // EPL_WPS_TYPES_H
