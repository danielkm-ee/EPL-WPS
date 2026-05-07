/* output.h
 *
 * Output-stage driver for the Powercore EDM bridge: four hardware-PWM
 * switches (SW_ENABLE, SW_HIGH_CURRENT_PHASE, SW_HIGH_VOLTAGE_PHASE,
 * OUTPUT_OVERCURRENT_SET) plus the EDM_FEEDBACK signal to the motion
 * controller and the discharge-event ISR for OUTPUT_OVERCURRENT.
 *
 * Singleton — no struct exposed; all state lives file-static in output.c.
 *
 * Boost coupling: setup_isofreq / setup_edge DO NOT ramp the boost DPOT.
 * The caller (main.c) must invoke boost_set_voltage() beforehand. These
 * functions only validate that the cal table is calibrated.
 *
 * GPIO IRQ: only one system-wide GPIO IRQ callback is allowed on the
 * RP2040. main.c owns that callback and dispatches by pin; the
 * dispatcher invokes output_overcurrent_isr() when OUTPUT_OVERCURRENT_PIN
 * drops. output_attach_isr() arms the falling-edge condition for the pin
 * but does NOT register the system callback.
 */

#ifndef EPL_WPS_OUTPUT_H
#define EPL_WPS_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>

#include "ctx.h"
#include "boost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Lifecycle ------------------------------------------------------- */

/* Cache PWM slice/channel mappings, configure the EDM_FEEDBACK PWM
 * (1 kHz, active-low), and force the four output switches into safe-OFF
 * GPIO state. Call once at boot, before any setup_* call. */
void output_init(void);

/* Bind the runtime context for use by output_overcurrent_isr() and arm
 * the falling-edge IRQ on OUTPUT_OVERCURRENT_PIN. The system-wide GPIO
 * callback must already be registered by main.c (or registered with the
 * first armed pin elsewhere). */
void output_attach_isr(main_ctx_t *ctx);

/* Discharge-event ISR. Called only by the GPIO dispatcher in main.c.
 * Iso-freq mode: snapshots ADC current+voltage, increments counters,
 * sets new_discharge_detected. Edge mode: latches edge_detected and
 * disables the output stage immediately. */
void output_overcurrent_isr(void);

/* --- Feedback PWM ---------------------------------------------------- */

/* Update the EDM_FEEDBACK duty (active-low: 0.0 = full power, 1.0 = no
 * load). Clamped to [0,1]. */
void output_feedback_set_duty(float duty);

/* --- Output stage configuration ------------------------------------- */

/* Low-level PWM (re)configure: reclaim the four output pads via
 * GPIO_FUNC_PWM, program phase-correct mode at the requested frequency,
 * load duty / HV pulse / overcurrent threshold levels, apply the
 * HV-phase counter offset, and enable slices selectively.
 *
 * SW_ENABLE and SW_HIGH_CURRENT_PHASE share PWM slice 5 — they cannot be
 * enabled independently, hence a single enable_main_slice flag. SW_HV
 * and OUTPUT_OVERCURRENT_SET share slice 4 (enable_hv_slice).
 *
 * Used directly by main.c during boost calibration (HV-only setup) and
 * indirectly via setup_isofreq / setup_edge. */
void output_stage_setup(float duty_cycle, int frequency_hz,
                        float oc_threshold_a,
                        bool enable_main_slice, bool enable_hv_slice,
                        float hv_pulse_us, float hv_offset);

/* Force the four output switches into safe-OFF state by reclaiming the
 * pads as SIO outputs. Does NOT change the EDM_FEEDBACK duty — callers
 * that want feedback off must call output_feedback_set_duty(0). */
void output_stage_disable(void);

/* High-level: configure for EDM iso-frequency mode using ctx->params.
 * Validates cal->calibrated; returns false if the cal table is not yet
 * built. Caller must have ramped the boost DPOT to ctx->params.init_voltage
 * via boost_set_voltage() before calling. Sets
 * ctx->discharge_rate_calc_interval_us and resets the success-rate timer
 * and current_discharge_count. */
bool output_setup_isofreq(main_ctx_t *ctx, const boost_cal_table_t *cal);

/* High-level: configure for edge-detection mode (main slice only at low
 * duty for workpiece probing). Validates cal->calibrated. Caller must
 * have ramped the boost DPOT to MIN_HIGH_VOLTAGE_PHASE_VOLTS. */
bool output_setup_edge(const boost_cal_table_t *cal);

/* --- Per-tick state-machine bodies ---------------------------------- */

/* One iteration of the EDM iso-frequency main-loop body. Drains any
 * pending discharge ADC sample, updates exponential averages, recomputes
 * the success rate at interval boundaries, applies pulse-skip pauses,
 * and honours discharge_count_target. Does NOT call output_setup_isofreq;
 * main.c handles initial mode prep before invoking this. */
void output_run_isofreq(main_ctx_t *ctx);

/* One iteration of the edge-detection main-loop body. Drains the
 * edge_detected flag latched by the ISR: signals the motion controller
 * via EDM_FEEDBACK, blocks for the protocol timeout, then clears the
 * flag. Does NOT call output_setup_edge; main.c handles initial mode
 * prep. After detection the output stays disabled until the device
 * cycles through IDLE. */
void output_run_edge(main_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_OUTPUT_H */
