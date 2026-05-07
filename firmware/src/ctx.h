/* ctx.h
 *
 * Central runtime context for the WPS firmware. main_ctx_t is the single
 * source of truth for device state, operating mode, machining parameters,
 * and discharge statistics. Pass by pointer to every function that needs
 * cross-module visibility.
 *
 * ISR-shared fields are individually volatile. Passing a non-volatile pointer
 * into ISR context is intentional: only the fields written by ISRs carry the
 * volatile qualifier, not the pointer itself.
 */

#ifndef EPL_WPS_CTX_H
#define EPL_WPS_CTX_H

#include <stdint.h>
#include <stdbool.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* --- State machine --- */
    volatile device_state_t      state;
    volatile mode_of_operation_t mode;
    volatile bool                mode_prep_done;

    /* --- Machining parameters (mutated by cmd module on SET_ALL_PARAMETERS) --- */
    volatile output_params_t     params;

    /* --- EDM iso-frequency discharge statistics (written by output ISR) --- */
    volatile bool     new_discharge_detected;
    volatile int      new_discharge_current_adc;
    volatile int      new_discharge_voltage_adc;
    volatile double   avg_discharge_current;
    volatile double   avg_discharge_voltage;
    volatile double   avg_discharge_success_rate;
    volatile double   discharge_success_rate;
    volatile int      current_discharge_count;
    volatile int      discharges_since_op_start;
    volatile bool     requested_discharges_reached;
    volatile uint32_t last_success_rate_calc_us;
    /* Calculated from machining frequency in output_setup_isofreq. */
    volatile int      discharge_rate_calc_interval_us;

    /* --- Edge-detection mode (written by output ISR) --- */
    volatile bool edge_detected;

    /* --- Periodic tick cursors (main loop only; not ISR-shared) --- */
    uint32_t last_periph_tick_ms;
    uint32_t last_telemetry_tick_ms;

    /* --- Feature flags (initialised from config.h constants) --- */
    bool allow_success_rate_pulse_skip;
    bool allow_power_setpoint_pulse_skip;
    bool periodic_telemetry_enabled;
} main_ctx_t;

/* Initialise all fields to safe defaults; copy feature flags from config.h. */
void ctx_init(main_ctx_t *ctx);

/* State and mode transitions. */
void ctx_set_state(main_ctx_t *ctx, device_state_t new_state);
void ctx_set_mode(main_ctx_t *ctx, mode_of_operation_t new_mode);

/* Reset discharge stats and prep flag. Called on IDLE entry. */
void ctx_reset_discharge_stats(main_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_CTX_H */
