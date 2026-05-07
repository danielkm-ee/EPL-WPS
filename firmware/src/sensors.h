/* sensors.h
 *
 * ADC-backed sensor drivers for the WPS firmware.
 *
 * Three physical sensors share ADC0..ADC2:
 *   - PMM input current  (GPIO 26 / ADC0, 200 mV/A, calibrated zero-offset)
 *   - Output voltage     (GPIO 27 / ADC1, 99.6:1 divider, no calibration)
 *   - Output current     (GPIO 28 / ADC2, TMCS1133 25 mV/A, calibrated)
 *
 * Module is a singleton: state (calibration offsets, ring buffer) lives as
 * file-static in sensors.c. Callers see a function-only API; struct
 * internals never leak out of the .c.
 *
 * ISR fast paths (raw-ADC reads + pure conversions) are documented per
 * function. Slow paths (ring-buffer push, multi-sample averages) must run
 * from the main loop only.
 */

#ifndef EPL_WPS_SENSORS_H
#define EPL_WPS_SENSORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Lifecycle -------------------------------------------------------- */

/* adc_init + adc_gpio_init for all three sensor pins. Call once at boot. */
void sensors_init(void);

/* Blocking zero-current calibration. Must run with no load on the sensor. */
void sensors_calibrate_input_current(void);
void sensors_calibrate_output_current(void);

/* --- Slow paths (main loop / periodic tick) --------------------------- */

/* Push one new PMM ADC sample into the running-average ring buffer. */
void  sensors_sample_input_current(void);

/* Running average of PMM input current, in amps. 0.0f until first fill. */
float sensors_avg_input_current_amps(void);

/* N-sample average of the output voltage divider, in volts. */
float sensors_read_output_voltage_averaged(int samples);

/* --- ISR fast paths --------------------------------------------------- */

/* Single ADC read of the output current/voltage channels. ISR-callable;
 * each call reselects the ADC mux before reading. */
int sensors_read_output_current_adc(void);
int sensors_read_output_voltage_adc(void);

/* Pure conversions from raw ADC counts to physical units. No I/O; safe to
 * call from any context. Negative results clamped to 0. */
float sensors_adc_to_discharge_current_amps(int adc);
float sensors_adc_to_discharge_voltage_volts(int adc);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_SENSORS_H */
