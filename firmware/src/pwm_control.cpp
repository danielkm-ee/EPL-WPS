/* pwm_control.cpp */

#include <Arduino.h>
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "pwm_control.h"
#include "types.h"
#include "../config.h"

// Output-stage slice/channel descriptors: SW_ENABLE (0), SW_HIGH_CURRENT_PHASE (1),
// SW_HIGH_VOLTAGE_PHASE (2), OUTPUT_OVERCURRENT_SET (3).
static PWMOutput output_stage[4];

// Feedback PWM descriptor.
static PWMOutput feedback_pwm;
static uint32_t  feedback_wrap = 0;

void pwm_setup_feedback(void) {
    feedback_pwm.slice   = pwm_gpio_to_slice_num(EDM_FEEDBACK_PIN);
    feedback_pwm.channel = pwm_gpio_to_channel(EDM_FEEDBACK_PIN);

    pwm_set_enabled(feedback_pwm.slice, false);
    feedback_wrap = PWM_BASE_CLOCK_FREQ
                    / (edmFeedbackPwmFrequency_Hz * EDM_FEEDBACK_PWM_CLOCK_DIVIDER);
    pwm_set_wrap(feedback_pwm.slice, feedback_wrap);
    pwm_set_clkdiv_int_frac(feedback_pwm.slice, EDM_FEEDBACK_PWM_CLOCK_DIVIDER, 0);
    pwm_set_chan_level(feedback_pwm.slice, feedback_pwm.channel, 0);
    pwm_set_enabled(feedback_pwm.slice, true);
}

void pwm_set_feedback_duty(double duty) {
    if (duty < 0.0) duty = 0.0;
    if (duty > 1.0) duty = 1.0;
    uint32_t level = (uint32_t)(feedback_wrap * duty);
    pwm_set_chan_level(feedback_pwm.slice, feedback_pwm.channel, level);
}

void pwm_setup_output_stage(double machining_duty_cycle,
                            int    machining_frequency_hz,
                            int    output_current_threshold_a,
                            bool   enable_switch,
                            bool   high_current_phase,
                            bool   high_voltage_phase,
                            int    hv_pulse_on_time_us,
                            double hv_pwm_offset) {
    gpio_set_function(SW_HIGH_VOLTAGE_PHASE_PIN,  GPIO_FUNC_PWM);
    gpio_set_function(SW_ENABLE_PIN,              GPIO_FUNC_PWM);
    gpio_set_function(SW_HIGH_CURRENT_PHASE_PIN,  GPIO_FUNC_PWM);
    gpio_set_function(OUTPUT_OVERCURRENT_SET_PIN, GPIO_FUNC_PWM);

    // Phase-correct counter counts up+down so the period doubles; wrap is
    // therefore f_clk / (f_out * 2).
    uint32_t wrap  = PWM_BASE_CLOCK_FREQ / (machining_frequency_hz * 2);
    uint32_t level = (uint32_t)(wrap * machining_duty_cycle);
    uint32_t hv_level = (uint32_t)((PWM_BASE_CLOCK_FREQ * 0.5f) * (hv_pulse_on_time_us / 1000000.0f));

    output_stage[0].slice   = pwm_gpio_to_slice_num(SW_ENABLE_PIN);
    output_stage[0].channel = pwm_gpio_to_channel(SW_ENABLE_PIN);
    output_stage[1].slice   = pwm_gpio_to_slice_num(SW_HIGH_CURRENT_PHASE_PIN);
    output_stage[1].channel = pwm_gpio_to_channel(SW_HIGH_CURRENT_PHASE_PIN);
    output_stage[2].slice   = pwm_gpio_to_slice_num(SW_HIGH_VOLTAGE_PHASE_PIN);
    output_stage[2].channel = pwm_gpio_to_channel(SW_HIGH_VOLTAGE_PHASE_PIN);
    output_stage[3].slice   = pwm_gpio_to_slice_num(OUTPUT_OVERCURRENT_SET_PIN);
    output_stage[3].channel = pwm_gpio_to_channel(OUTPUT_OVERCURRENT_SET_PIN);

    // Invert the P-channel MOSFET drives. Slice 1 pairs N-ch SW_ENABLE with
    // P-ch SW_HIGH_CURRENT_PHASE; slice 2 pairs P-ch SW_HIGH_VOLTAGE_PHASE
    // with the non-inverted OUTPUT_OVERCURRENT_SET.
    pwm_set_output_polarity(output_stage[1].slice, false, true);
    pwm_set_output_polarity(output_stage[2].slice, true,  false);

    for (int i = 0; i < 4; i++) {
        pwm_set_enabled(output_stage[i].slice, false);
        pwm_set_wrap(output_stage[i].slice, wrap);
        pwm_set_clkdiv_int_frac(output_stage[i].slice, 1, 0);
        pwm_set_phase_correct(output_stage[i].slice, true);
    }

    pwm_set_chan_level(output_stage[0].slice, output_stage[0].channel, level);
    pwm_set_chan_level(output_stage[1].slice, output_stage[1].channel, level);
    pwm_set_chan_level(output_stage[2].slice, output_stage[2].channel,
                       high_voltage_phase ? hv_level : 0);

    // Overcurrent threshold as a PWM duty that the comparator averages.
    float    v_thresh     = (ISENSE_OUTPUT_V_PER_AMP * output_current_threshold_a) / 2.5f;
    uint32_t thresh_level = (uint32_t)((v_thresh / 3.3f) * wrap);
    pwm_set_chan_level(output_stage[3].slice, output_stage[3].channel, thresh_level);

    for (int i = 0; i < 4; i++) {
        pwm_set_counter(output_stage[i].slice, 0);
    }
    // Offset the HV-phase counter to prevent shoot-through.
    pwm_set_counter(output_stage[2].slice, (uint32_t)(wrap * hv_pwm_offset));

    pwm_set_enabled(output_stage[0].slice, enable_switch);
    pwm_set_enabled(output_stage[1].slice, high_current_phase);
    pwm_set_enabled(output_stage[2].slice, high_voltage_phase);
    pwm_set_enabled(output_stage[3].slice, true);
}

void pwm_disable_output_stage(void) {
    pinMode(SW_ENABLE_PIN, OUTPUT);
    gpio_put(SW_ENABLE_PIN, false);               // N-ch OFF = LOW
    pinMode(SW_HIGH_CURRENT_PHASE_PIN, OUTPUT);
    gpio_put(SW_HIGH_CURRENT_PHASE_PIN, true);    // P-ch OFF = HIGH
    pinMode(SW_HIGH_VOLTAGE_PHASE_PIN, OUTPUT);
    gpio_put(SW_HIGH_VOLTAGE_PHASE_PIN, true);    // P-ch OFF = HIGH

    pwm_set_feedback_duty(0.0);
}
