/* sensors.c
 *
 * ADC sensor implementation. All module state is file-static; the API is
 * a free-function singleton (see sensors.h).
 *
 * ADC channel mapping is positional on the RP2040: GPIO 26..28 → ADC 0..2.
 * `adc_select_input(pin - 26)` is the canonical conversion.
 */

#include "pico/stdlib.h"
#include "hardware/adc.h"

#include "sensors.h"
#include "../config.h"

/* --- Module-static state --------------------------------------------- */

/* Calibration offsets are written by main-loop calibration routines and
 * read by ISR-callable conversion helpers. volatile guards against the
 * compiler caching them across the ISR boundary. */
static volatile int s_pmm_zero_offset       = 0;
static volatile int s_output_current_offset = 0;

/* PMM input-current ring buffer. Single-producer (periodic tick), single-
 * consumer (main loop average). Not ISR-shared. */
static float s_input_current_buf[DEVICE_CURRENT_BUFFER_SIZE];
static int   s_input_current_head  = 0;
static int   s_input_current_count = 0;

/* --- Internal helpers ------------------------------------------------- */

/* Select the ADC mux for a given GPIO and return one fresh sample. */
static inline int sensors_read_pin_adc(int pin)
{
    adc_select_input(pin - 26);
    return (int)adc_read();
}

/* --- Lifecycle -------------------------------------------------------- */

void sensors_init(void)
{
    adc_init();
    adc_gpio_init(PMM_ISENSE_PIN);
    adc_gpio_init(OUTPUT_VSENSE_PIN);
    adc_gpio_init(OUTPUT_ISENSE_PIN);
}

void sensors_calibrate_input_current(void)
{
    long sum = 0;
    for (int i = 0; i < PMM_CALIBRATION_SAMPLES; i++) {
        sum += sensors_read_pin_adc(PMM_ISENSE_PIN);
        sleep_ms(PMM_CALIBRATION_DELAY_MS);
    }
    s_pmm_zero_offset = (int)(sum / PMM_CALIBRATION_SAMPLES);
}

void sensors_calibrate_output_current(void)
{
    long sum = 0;
    for (int i = 0; i < OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES; i++) {
        sum += sensors_read_pin_adc(OUTPUT_ISENSE_PIN);
        sleep_ms(OUTPUT_CURRENT_SENSOR_CALIBRATION_DELAY_MS);
    }
    s_output_current_offset = (int)(sum / OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES);
}

/* --- Slow paths ------------------------------------------------------- */

void sensors_sample_input_current(void)
{
    int adc = sensors_read_pin_adc(PMM_ISENSE_PIN);
    float a = (adc - s_pmm_zero_offset) * ADC_TO_VOLTS / PMM_ISENSE_V_PER_AMP;
    if (a < 0.0f) a = 0.0f;

    s_input_current_buf[s_input_current_head] = a;
    s_input_current_head = (s_input_current_head + 1) % DEVICE_CURRENT_BUFFER_SIZE;
    if (s_input_current_count < DEVICE_CURRENT_BUFFER_SIZE)
        s_input_current_count++;
}

float sensors_avg_input_current_amps(void)
{
    if (s_input_current_count == 0)
        return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < s_input_current_count; i++)
        sum += s_input_current_buf[i];
    return sum / (float)s_input_current_count;
}

float sensors_read_output_voltage_averaged(int samples)
{
    if (samples <= 0)
        return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < samples; i++)
        sum += sensors_read_pin_adc(OUTPUT_VSENSE_PIN) * OUTPUT_VOLTAGE_SCALE;
    return sum / (float)samples;
}

/* --- ISR fast paths --------------------------------------------------- */

int sensors_read_output_current_adc(void)
{
    return sensors_read_pin_adc(OUTPUT_ISENSE_PIN);
}

int sensors_read_output_voltage_adc(void)
{
    return sensors_read_pin_adc(OUTPUT_VSENSE_PIN);
}

float sensors_adc_to_discharge_current_amps(int adc)
{
    float a = (adc - s_output_current_offset) * ADC_TO_VOLTS * (float)ISENSE_OUTPUT_AMPS_PER_VOLT;
    return a < 0.0f ? 0.0f : a;
}

float sensors_adc_to_discharge_voltage_volts(int adc)
{
    float v = adc * OUTPUT_VOLTAGE_SCALE;
    return v < 0.0f ? 0.0f : v;
}
