/* feedback_test.c
 *
 * Standalone sweep test for the EDM_FEEDBACK PWM output (GP3).
 * Continuously sweeps frequency between FREQ_MIN_HZ (200 Hz) and
 * FREQ_MAX_HZ (400 Hz) at a fixed 50% duty cycle, mirroring the
 * encoding used by the main firmware. Progress is printed over USB CDC.
 *
 * Build:  cmake -DPICO_SDK_PATH=<path> .. && make
 * Flash:  drag-drop feedback_test.uf2, or use picotool / openocd
 */

#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

/*=============================================================================
 * Constants (mirrors firmware/config.h)
 *===========================================================================*/
#define FEEDBACK_PIN        3
#define LED_PIN             25
#define PWM_BASE_CLOCK_HZ   133000000u
#define CLOCK_DIV           64u
#define FREQ_MIN_HZ         200u
#define FREQ_MAX_HZ         400u
#define DUTY                0.5f

/* 100 steps * 50 ms = 5 s per half-sweep (10 s round-trip) */
#define SWEEP_STEPS         100
#define SWEEP_STEP_MS       50

/*=============================================================================
 * PWM helpers
 *===========================================================================*/
static uint g_slice;
static uint g_channel;

static void feedback_init(void)
{
    gpio_set_function(FEEDBACK_PIN, GPIO_FUNC_PWM);
    g_slice   = pwm_gpio_to_slice_num(FEEDBACK_PIN);
    g_channel = pwm_gpio_to_channel(FEEDBACK_PIN);
    pwm_set_clkdiv_int_frac(g_slice, CLOCK_DIV, 0);
    pwm_set_enabled(g_slice, false);
}

static void feedback_set_freq(uint32_t freq_hz)
{
    uint32_t wrap  = PWM_BASE_CLOCK_HZ / (freq_hz * CLOCK_DIV);
    uint32_t level = (uint32_t)((float)wrap * DUTY);
    pwm_set_wrap(g_slice, (uint16_t)wrap);
    pwm_set_chan_level(g_slice, g_channel, (uint16_t)level);
    pwm_set_enabled(g_slice, true);
}

/*=============================================================================
 * Entry point
 *===========================================================================*/
int main(void)
{
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);
    feedback_init();

    printf("EDM_FEEDBACK sweep test: GP%d, %u Hz <-> %u Hz\n",
           FEEDBACK_PIN, FREQ_MIN_HZ, FREQ_MAX_HZ);

    while (true) {
        /* Rising sweep: ratio 0.0 -> 1.0 */
        for (int i = 0; i <= SWEEP_STEPS; i++) {
            float    ratio   = (float)i / (float)SWEEP_STEPS;
            uint32_t freq_hz = FREQ_MIN_HZ
                               + (uint32_t)(ratio * (float)(FREQ_MAX_HZ - FREQ_MIN_HZ));
            feedback_set_freq(freq_hz);
            printf("ratio=%.2f  freq=%lu Hz\n", (double)ratio, (unsigned long)freq_hz);
            sleep_ms(SWEEP_STEP_MS);
        }
        /* Falling sweep: ratio 1.0 -> 0.0 */
        for (int i = SWEEP_STEPS; i >= 0; i--) {
            float    ratio   = (float)i / (float)SWEEP_STEPS;
            uint32_t freq_hz = FREQ_MIN_HZ
                               + (uint32_t)(ratio * (float)(FREQ_MAX_HZ - FREQ_MIN_HZ));
            feedback_set_freq(freq_hz);
            printf("ratio=%.2f  freq=%lu Hz\n", (double)ratio, (unsigned long)freq_hz);
            sleep_ms(SWEEP_STEP_MS);
        }
    }
}
