/* sensors.h
 *
 * ADC-backed sensor drivers: PMM input current, output current, and
 * output voltage. Owns calibration offsets and the input-current
 * running-average buffer.
 */

#ifndef EPL_WPS_SENSORS_H
#define EPL_WPS_SENSORS_H

#include <stdint.h>

void  sensors_setup_adc(void);

// Blocking zero-current calibration. Must run with no load on the sensor.
void  sensors_calibrate_input_current(void);
void  sensors_calibrate_output_current(void);

// Push one new PMM ADC sample into the running-average buffer.
void  sensors_sample_input_current(void);

// Running-average buffer of PMM input current (A). Zero until first fill.
float sensors_avg_input_current(void);

// Convert discharge-event ADC samples (captured in the overcurrent ISR).
double sensors_adc_to_discharge_current(int adc);
double sensors_adc_to_discharge_voltage(int adc);

// Averaged read of the boost-converter output voltage (V).
int   sensors_read_output_voltage_averaged(int samples);

#endif // EPL_WPS_SENSORS_H
