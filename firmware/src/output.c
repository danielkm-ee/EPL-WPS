/* output.c
 *
 * Output-stage driver: PWM switching, EDM_FEEDBACK signal, and the
 * OUTPUT_OVERCURRENT-driven discharge capture ISR.
 *
 * Singleton — file-static state. ISR-shared fields live in main_ctx_t
 * (bound via output_attach_isr) and are individually volatile.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "output.h"
#include "sensors.h"
#include "ctx.h"
#include "../config.h"

/* --- File-static state ----------------------------------------------- */

/* Bound runtime context for the ISR. NULL until output_attach_isr().
 * main_ctx_t fields touched from the ISR are individually volatile. */
static main_ctx_t *g_ctx = NULL;

/* PWM slice/channel descriptors — populated once in output_init(). */
static pwm_output_t g_sw_en;
static pwm_output_t g_sw_hc;
static pwm_output_t g_sw_hv;
static pwm_output_t g_oc_set;
static pwm_output_t g_feedback;
static uint32_t     g_feedback_wrap = 0;

/* --- Internal helpers ------------------------------------------------ */

/* Reclaim a pad from the PWM peripheral and drive it as a static GPIO. */
static void output_safe_gpio(int pin, bool level)
{
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, level);
}

static void output_cache_pwm_descriptors(void)
{
    g_sw_en.slice    = pwm_gpio_to_slice_num(SW_ENABLE_PIN);
    g_sw_en.channel  = pwm_gpio_to_channel(SW_ENABLE_PIN);
    g_sw_hc.slice    = pwm_gpio_to_slice_num(SW_HIGH_CURRENT_PHASE_PIN);
    g_sw_hc.channel  = pwm_gpio_to_channel(SW_HIGH_CURRENT_PHASE_PIN);
    g_sw_hv.slice    = pwm_gpio_to_slice_num(SW_HIGH_VOLTAGE_PHASE_PIN);
    g_sw_hv.channel  = pwm_gpio_to_channel(SW_HIGH_VOLTAGE_PHASE_PIN);
    g_oc_set.slice   = pwm_gpio_to_slice_num(OUTPUT_OVERCURRENT_SET_PIN);
    g_oc_set.channel = pwm_gpio_to_channel(OUTPUT_OVERCURRENT_SET_PIN);
    g_feedback.slice   = pwm_gpio_to_slice_num(EDM_FEEDBACK_PIN);
    g_feedback.channel = pwm_gpio_to_channel(EDM_FEEDBACK_PIN);
}

/* --- Lifecycle -------------------------------------------------------- */

void output_init(void)
{
    output_cache_pwm_descriptors();

    /* EDM_FEEDBACK: 1 kHz active-low PWM, runs continuously. */
    gpio_set_function(EDM_FEEDBACK_PIN, GPIO_FUNC_PWM);
    pwm_set_enabled(g_feedback.slice, false);
    g_feedback_wrap = PWM_BASE_CLOCK_FREQ
                      / (EDM_FEEDBACK_PWM_FREQ_HZ * EDM_FEEDBACK_PWM_CLOCK_DIVIDER);
    pwm_set_wrap(g_feedback.slice, g_feedback_wrap);
    pwm_set_clkdiv_int_frac(g_feedback.slice, EDM_FEEDBACK_PWM_CLOCK_DIVIDER, 0);
    pwm_set_chan_level(g_feedback.slice, g_feedback.channel, 0);
    pwm_set_enabled(g_feedback.slice, true);

    /* Output switches: safe-OFF before the PWM peripheral ever owns the
     * pads. */
    output_stage_disable();
}

void output_attach_isr(main_ctx_t *ctx)
{
    g_ctx = ctx;
    /* Arm the falling-edge condition. The system-wide callback is owned
     * by main.c; arming a pin without a registered callback is benign. */
    gpio_set_irq_enabled(OUTPUT_OVERCURRENT_PIN, GPIO_IRQ_EDGE_FALL, true);
}

void output_overcurrent_isr(void)
{
    if (!g_ctx) return;

    switch (g_ctx->mode) {
    case EDM_ISOFREQUENCY_MODE:
        g_ctx->new_discharge_current_adc = sensors_read_output_current_adc();
        g_ctx->new_discharge_voltage_adc = sensors_read_output_voltage_adc();
        g_ctx->current_discharge_count++;
        g_ctx->discharges_since_op_start++;
        g_ctx->new_discharge_detected = true;
        break;
    case EDGE_DETECTION_MODE:
        if (g_ctx->edge_detected) break;     /* already pending */
        g_ctx->edge_detected = true;
        output_stage_disable();              /* immediate safety */
        break;
    }
}

/* --- Feedback PWM ---------------------------------------------------- */

void output_feedback_set_duty(float duty)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    uint32_t level = (uint32_t)(g_feedback_wrap * duty);
    pwm_set_chan_level(g_feedback.slice, g_feedback.channel, level);
}

/* --- Output stage configuration ------------------------------------- */

void output_stage_setup(float duty_cycle, int frequency_hz,
                        float oc_threshold_a,
                        bool enable_main_slice, bool enable_hv_slice,
                        float hv_pulse_us, float hv_offset)
{
    /* Reclaim the pads from any prior SIO state. */
    gpio_set_function(SW_HIGH_VOLTAGE_PHASE_PIN,  GPIO_FUNC_PWM);
    gpio_set_function(SW_ENABLE_PIN,              GPIO_FUNC_PWM);
    gpio_set_function(SW_HIGH_CURRENT_PHASE_PIN,  GPIO_FUNC_PWM);
    gpio_set_function(OUTPUT_OVERCURRENT_SET_PIN, GPIO_FUNC_PWM);

    /* Phase-correct counter counts up + down: a full output period is
     * 2 * wrap base-clock ticks. */
    uint32_t wrap     = PWM_BASE_CLOCK_FREQ / ((uint32_t)frequency_hz * 2u);
    uint32_t level    = (uint32_t)((float)wrap * duty_cycle);
    uint32_t hv_level = (uint32_t)(((float)PWM_BASE_CLOCK_FREQ * 0.5f)
                                   * (hv_pulse_us / 1000000.0f));

    /* Per-channel polarity:
     *   slice 5 (main): A=SW_ENABLE        N-ch, normal
     *                   B=SW_HIGH_CURRENT  P-ch, inverted
     *   slice 4 (HV):   A=SW_HIGH_VOLTAGE  P-ch, inverted
     *                   B=OC_SET           normal */
    pwm_set_output_polarity(g_sw_en.slice, false, true);
    pwm_set_output_polarity(g_sw_hv.slice, true,  false);

    /* Configure both slices identically. */
    uint32_t slices[2] = { g_sw_en.slice, g_sw_hv.slice };
    for (int i = 0; i < 2; i++) {
        pwm_set_enabled(slices[i], false);
        pwm_set_wrap(slices[i], (uint16_t)wrap);
        pwm_set_clkdiv_int_frac(slices[i], 1, 0);
        pwm_set_phase_correct(slices[i], true);
    }

    pwm_set_chan_level(g_sw_en.slice, g_sw_en.channel, level);
    pwm_set_chan_level(g_sw_hc.slice, g_sw_hc.channel, level);
    pwm_set_chan_level(g_sw_hv.slice, g_sw_hv.channel,
                       enable_hv_slice ? hv_level : 0u);

    /* Overcurrent threshold via PWM-as-DAC. The downstream RC averages
     * this PWM and the comparator trips when output current exceeds the
     * resulting voltage threshold. */
    float    v_thresh     = (ISENSE_OUTPUT_V_PER_AMP * oc_threshold_a) / 2.5f;
    uint32_t thresh_level = (uint32_t)((v_thresh / 3.3f) * (float)wrap);
    pwm_set_chan_level(g_oc_set.slice, g_oc_set.channel, thresh_level);

    /* Reset main slice counter; offset HV slice by hv_offset * wrap so
     * the HV pre-charge fires before the next machining pulse, leaving a
     * settling gap that prevents shoot-through with SW_ENABLE. */
    pwm_set_counter(g_sw_en.slice, 0);
    pwm_set_counter(g_sw_hv.slice, (uint16_t)((float)wrap * hv_offset));

    pwm_set_enabled(g_sw_en.slice, enable_main_slice);
    pwm_set_enabled(g_sw_hv.slice, enable_hv_slice);
}

void output_stage_disable(void)
{
    output_safe_gpio(SW_ENABLE_PIN,             false);  /* N-ch OFF = LOW  */
    output_safe_gpio(SW_HIGH_CURRENT_PHASE_PIN, true);   /* P-ch OFF = HIGH */
    output_safe_gpio(SW_HIGH_VOLTAGE_PHASE_PIN, true);   /* P-ch OFF = HIGH */
    /* OUTPUT_OVERCURRENT_SET stays on PWM — it's a slowly-changing
     * threshold reference, not a switch. The downstream comparator reads
     * its time-averaged level; leaving the PWM running keeps the
     * threshold valid for any subsequent re-enable. */
}

bool output_setup_isofreq(main_ctx_t *ctx, const boost_cal_table_t *cal)
{
    if (!ctx || !cal || !cal->calibrated) return false;

    output_stage_setup(ctx->params.duty_cycle,
                       (int)ctx->params.frequency_hz,
                       (float)DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                       true, true,
                       (float)EDM_ISOFREQ_HV_PULSE_ON_TIME_US,
                       (float)EDM_ISOFREQ_HV_PWM_OFFSET);

    /* Sampling window for success-rate calc: time for one batch of
     * DISCHARGES_PER_CALC_INTERVAL discharges at the machining frequency. */
    ctx->discharge_rate_calc_interval_us =
        (int)((float)DISCHARGES_PER_CALC_INTERVAL
              * (1000000.0f / ctx->params.frequency_hz));
    ctx->current_discharge_count   = 0;
    ctx->last_success_rate_calc_us = to_us_since_boot(get_absolute_time());

    return true;
}

bool output_setup_edge(const boost_cal_table_t *cal)
{
    if (!cal || !cal->calibrated) return false;

    /* Edge detection: main slice only at low duty for probing pulses;
     * HV slice off. Matches stale firmware setup_edge_output_pwm. */
    output_stage_setup(0.05f, 10000,
                       (float)DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                       true, false,
                       1.0f, 0.25f);
    return true;
}

/* --- Per-tick state-machine bodies ---------------------------------- */

/* Re-enable iso-freq PWM after a pulse-skip pause. Boost voltage is
 * unchanged during the pause, so no DPOT touch is needed. */
static void output_isofreq_rearm(const main_ctx_t *ctx)
{
    output_stage_setup(ctx->params.duty_cycle,
                       (int)ctx->params.frequency_hz,
                       (float)DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                       true, true,
                       (float)EDM_ISOFREQ_HV_PULSE_ON_TIME_US,
                       (float)EDM_ISOFREQ_HV_PWM_OFFSET);
}

void output_run_isofreq(main_ctx_t *ctx)
{
    if (!ctx) return;

    /* --- Drain pending discharge ADC sample --- */
    if (ctx->new_discharge_detected) {
        ctx->new_discharge_detected = false;
        float i = sensors_adc_to_discharge_current_amps(ctx->new_discharge_current_adc);
        float v = sensors_adc_to_discharge_voltage_volts(ctx->new_discharge_voltage_adc);
        ctx->avg_discharge_current = (0.90 * ctx->avg_discharge_current) + (0.10 * (double)i);
        ctx->avg_discharge_voltage = (0.90 * ctx->avg_discharge_voltage) + (0.10 * (double)v);
    }

    /* --- Honour discharge_count_target latch --- */
    if (ctx->params.discharge_count_target != 0
        && ctx->discharges_since_op_start >= ctx->params.discharge_count_target) {
        if (!ctx->requested_discharges_reached) {
            printf("Number of discharges requested reached: %d\n",
                   ctx->discharges_since_op_start);
            output_stage_disable();
        }
        output_feedback_set_duty(0.0f);
        ctx->requested_discharges_reached = true;
        return;
    }
    ctx->requested_discharges_reached = false;

    /* --- Recompute success rate at interval boundaries --- */
    uint32_t now_us = to_us_since_boot(get_absolute_time());
    bool interval_elapsed = (now_us - ctx->last_success_rate_calc_us
                             > (uint32_t)ctx->discharge_rate_calc_interval_us);

    if (interval_elapsed
        || ctx->current_discharge_count >= DISCHARGES_PER_CALC_INTERVAL) {
        ctx->discharge_success_rate = (double)ctx->current_discharge_count
                                       / (double)DISCHARGES_PER_CALC_INTERVAL;
        ctx->avg_discharge_success_rate = (0.99 * ctx->avg_discharge_success_rate)
                                           + (0.01 * ctx->discharge_success_rate);

        /* Pulse-skip: too many concurrent discharges (gap shorted). */
        if (ctx->discharge_success_rate >= MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD
            && ctx->allow_success_rate_pulse_skip) {
            output_stage_disable();
            printf("Pulse skipping: discharge success rate threshold exceeded\n");
            sleep_ms(250);
            ctx->discharge_success_rate = 0.0;
            printf("Pulse skipping complete\n");
            output_isofreq_rearm(ctx);
        }

        /* Pulse-skip: input power above thermal limit. */
        if ((sensors_avg_input_current_amps() * INPUT_SUPPLY_VOLTAGE)
              > MAX_INPUT_POWER_SETPOINT_WATTS
            && ctx->allow_power_setpoint_pulse_skip) {
            output_stage_disable();
            printf("Pulse skipping: power setpoint exceeded\n");
            sleep_ms(10);
            printf("Pulse skipping complete\n");
            output_isofreq_rearm(ctx);
        }

        ctx->current_discharge_count   = 0;
        ctx->last_success_rate_calc_us = now_us;
    }
}

void output_run_edge(main_ctx_t *ctx)
{
    if (!ctx || !ctx->edge_detected) return;

    /* ISR has already disabled the output stage; drive the protocol
     * sequence to the motion controller, then clear the latch. The
     * output stays disabled until handle_idle_state in main.c clears
     * mode_prep_done and re-runs setup. */
    output_feedback_set_duty(1.0f);
    printf("EDGE DETECTED\n");
    sleep_ms(1000);
    output_feedback_set_duty(0.0f);
    sleep_ms(1000);
    ctx->edge_detected = false;
}
