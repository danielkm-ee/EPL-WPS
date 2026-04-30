/* sensors.cpp */

#include <Arduino.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "sensors.h"
#include "../config.h"

static volatile int   pmm_zero_offset       = 0;
static volatile int   output_current_offset = 0;

static float input_current_buf[DEVICE_CURRENT_BUFFER_SIZE];
static int   input_current_head  = 0;
static int   input_current_count = 0;

void sensors_setup_adc(void) {
    analogReadResolution(ADC_RESOLUTION_BITS);
}

void sensors_calibrate_input_current(void) {
    long sum = 0;
    for (int i = 0; i < PMM_CALIBRATION_SAMPLES; i++) {
        sum += analogRead(PMM_ISENSE_PIN);
        delay(PMM_CALIBRATION_DELAY_MS);
    }
    pmm_zero_offset = sum / PMM_CALIBRATION_SAMPLES;
}

void sensors_sample_input_current(void) {
    float a = (analogRead(PMM_ISENSE_PIN) - pmm_zero_offset) * ADC_TO_VOLTS / PMM_ISENSE_V_PER_AMP;
    if (a < 0.0f) a = 0.0f;
    input_current_buf[input_current_head] = a;
    input_current_head = (input_current_head + 1) % DEVICE_CURRENT_BUFFER_SIZE;
    if (input_current_count < DEVICE_CURRENT_BUFFER_SIZE)
        input_current_count++;
}

float sensors_avg_input_current(void) {
    if (input_current_count == 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < input_current_count; i++)
        sum += input_current_buf[i];
    return sum / input_current_count;
}

void sensors_calibrate_output_current(void) {
    long sum = 0;
    for (int i = 0; i < OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES; i++) {
        sum += analogRead(OUTPUT_ISENSE_PIN);
        delay(OUTPUT_CURRENT_SENSOR_CALIBRATION_DELAY_MS);
    }
    output_current_offset = sum / OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES;
}


double sensors_adc_to_discharge_current(int adc) {
    double a = (adc - output_current_offset) * ADC_TO_VOLTS * ISENSE_OUTPUT_AMPS_PER_VOLT;
    return a < 0.0 ? 0.0 : a;
}

double sensors_adc_to_discharge_voltage(int adc) {
    double v = adc * OUTPUT_VOLTAGE_SCALE;
    return v < 0.0 ? 0.0 : v;
}

float sensors_read_output_voltage_averaged(int samples) {
    float sum = 0.0f;
    for (int i = 0; i < samples; i++) {
        sum += analogRead(OUTPUT_VSENSE_PIN) * OUTPUT_VOLTAGE_SCALE;
    }
    return sum / samples;
}
