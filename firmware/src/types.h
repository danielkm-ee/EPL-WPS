/* types.h
 *
 * Shared enumerations and structs for the WPS firmware.
 * Pure C11; extern "C" guarded for inclusion from C++ translation units.
 */

#ifndef EPL_WPS_TYPES_H
#define EPL_WPS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STARTUP,
    FAULT,
    OPERATING,
    IDLE
} device_state_t;

typedef enum {
    EDM_ISOFREQUENCY_MODE,
    EDGE_DETECTION_MODE
} mode_of_operation_t;

typedef enum {
    PMM_FAULT_TYPE,
    BOOST_CONVERTER_PGOOD_FAULT,
    POWER_OUT_OF_RANGE_FAULT,
    HIGH_VOLTAGE_PHASE_SETUP_FAULT
} fault_type_t;

/* Parameter payload validated by cmd module before commit to main_ctx_t. */
typedef struct {
    int   discharge_count_target;   /* 0 = infinite */
    float duty_cycle;               /* 0.01 .. 0.12 */
    float frequency_hz;             /* 5000 .. 10000 */
    float init_voltage;             /* 64 .. 100 V */
} output_params_t;

/* RP2040 PWM slice/channel pair. Private to output.c; defined here so
 * output_t can be embedded without an opaque allocation. */
typedef struct {
    uint32_t slice;
    uint32_t channel;
} pwm_output_t;

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_TYPES_H */
