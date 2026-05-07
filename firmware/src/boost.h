/* boost.h
 *
 * Driver for the boost-converter module's digital potentiometer (TPL0401B
 * over I2C).
 * Manages a calibration table to update wiper position based on target
 * voltage. Relies on updates from output sense ADC.
 */

#ifndef EPL_WPS_BOOST_H
#define EPL_WPS_BOOST_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"
#include "../config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOST_CAL_OK,
    BOOST_CAL_TIMEOUT,
    BOOST_CAL_INVALID_INPUT,
} boost_cal_status_t;

typedef struct {
    float   voltage;
    uint8_t wiper;
} boost_cal_point_t;

/* Assumes BOOST_DPOT_POSITION_MIN == 0; one entry per wiper step. */
typedef struct {
    boost_cal_point_t entries[BOOST_DPOT_POSITION_MAX + 1];
    uint8_t           count;
    bool              calibrated;
} boost_cal_table_t;

typedef struct boost_dpot boost_dpot_t;

/* DPOT lifecycle */
boost_dpot_t* boost_dpot_create(i2c_inst_t* i2c_port, uint8_t addr, uint8_t reg);
void boost_setup_i2c(boost_dpot_t* dpot, uint8_t sda_pin, uint8_t scl_pin, uint16_t baud_hz);

/* DPOT operations */
void    boost_dpot_write_position(boost_dpot_t* dpot, uint8_t position);
uint8_t boost_dpot_read_position(boost_dpot_t* dpot);
uint8_t boost_dpot_clamp_position(uint8_t position);

/* DPOT accessors */
uint8_t boost_dpot_read_check(boost_dpot_t* dpot);
uint8_t boost_dpot_write_check(boost_dpot_t* dpot);
uint8_t boost_dpot_get_position(boost_dpot_t* dpot);
float   boost_dpot_get_voltage(const boost_dpot_t* dpot, const boost_cal_table_t* tbl);

/* Calibration — sweep wiper positions and record output voltage at each step.
 * settle_ms: delay after each wiper write before sampling the ADC. */
boost_cal_status_t boost_cal_build(boost_dpot_t* dpot, boost_cal_table_t* tbl,
                                   int adc_samples, uint32_t settle_ms);

/* Binary search for the wiper position whose calibrated voltage is closest to
 * target_v. Assumes monotonically increasing voltage with wiper position. */
uint8_t boost_cal_lookup_wiper(const boost_cal_table_t* tbl, float target_v);

/* Look up target_v in the cal table and ramp the wiper to it one step at a
 * time, sleeping ramp_step_ms between each position change. */
void boost_set_voltage(boost_dpot_t* dpot, const boost_cal_table_t* tbl,
                       float target_v, uint32_t ramp_step_ms);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_BOOST_H */
