/* sensors.cpp */

#include <Arduino.h>
#include "sensors.h"
#include "../config.h"

static volatile int   pmm_zero_offset       = 0;
static volatile int   output_current_offset = 0;

static volatile float input_buffer[DEVICE_CURRENT_BUFFER_SIZE];
static volatile int   input_buffer_index   = 0;
static volatile bool  input_buffer_filled  = false;
static volatile float input_current_avg    = 0.0f;

void sensors_setup_adc(void) {
    analogReadResolution(ADC_RESOLUTION_BITS);
}

void sensors_calibrate_input_current(void) {
    long sum = 0;
    for (int i = 0; i < PMM_CALIBRATION_SAMPLES; i++) {
        sum += analogRead(PMM_ISENSE);
        delay(PMM_CALIBRATION_DELAY_MS);
    }
    pmm_zero_offset = sum / PMM_CALIBRATION_SAMPLES;
}

void sensors_calibrate_output_current(void) {
    long sum = 0;
    for (int i = 0; i < OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES; i++) {
        sum += analogRead(OUTPUT_ISENSE);
        delay(OUTPUT_CURRENT_SENSOR_CALIBRATION_DELAY_MS);
    }
    output_current_offset = sum / OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES;
}

void sensors_sample_input_current(void) {
    int adc = analogRead(PMM_ISENSE);
    float amps = ((adc - pmm_zero_offset) * ADC_TO_VOLTS) / PMM_ISENSE_V_PER_AMP;

    input_buffer[input_buffer_index] = amps;
    input_buffer_index = (input_buffer_index + 1) % DEVICE_CURRENT_BUFFER_SIZE;
    if (input_buffer_index == 0) {
        input_buffer_filled = true;
    }

    if (!input_buffer_filled) {
        input_current_avg = 0.0f;
        return;
    }
    float sum = 0.0f;
    for (int i = 0; i < DEVICE_CURRENT_BUFFER_SIZE; i++) {
        sum += input_buffer[i];
    }
    input_current_avg = sum / DEVICE_CURRENT_BUFFER_SIZE;
}

float sensors_avg_input_current(void) {
    return input_current_avg;
}

double sensors_adc_to_discharge_current(int adc) {
    double a = (adc - output_current_offset) * ADC_TO_VOLTS * ISENSE_OUTPUT_AMPS_PER_VOLT;
    return a < 0.0 ? 0.0 : a;
}

double sensors_adc_to_discharge_voltage(int adc) {
    double v = adc * OUTPUT_VOLTAGE_SCALE;
    return v < 0.0 ? 0.0 : v;
}

int sensors_read_output_voltage_averaged(int samples) {
    int sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += (int)(analogRead(OUTPUT_VSENSE) * OUTPUT_VOLTAGE_SCALE);
    }
    return sum / samples;
}
