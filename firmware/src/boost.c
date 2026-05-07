/* boost.c */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "boost.h"
#include "sensors.h"
#include "../config.h"

struct boost_dpot {
    i2c_inst_t *port;
    uint8_t     addr;
    uint8_t     reg;
    uint8_t     position;
    uint8_t     write_status;
    uint8_t     read_status;
};

boost_dpot_t* boost_dpot_create(i2c_inst_t* i2c_port, uint8_t addr, uint8_t reg) {
    boost_dpot_t* dpot = (boost_dpot_t*)malloc(sizeof(struct boost_dpot));
    if (dpot) {
        dpot->port         = i2c_port;
        dpot->addr         = addr;
        dpot->reg          = reg;
        dpot->position     = 1;
        dpot->write_status = 0;
        dpot->read_status  = 0;
    }
    return dpot;
}

void boost_setup_i2c(boost_dpot_t* dpot, uint8_t sda_pin, uint8_t scl_pin, uint16_t baud_hz) {
    i2c_init(dpot->port, baud_hz);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
}

uint8_t boost_dpot_read_check(boost_dpot_t* dpot)   { return dpot->read_status; }
uint8_t boost_dpot_write_check(boost_dpot_t* dpot)  { return dpot->write_status; }
uint8_t boost_dpot_get_position(boost_dpot_t* dpot) { return dpot->position; }

uint8_t boost_dpot_clamp_position(uint8_t position) {
    position = (position < BOOST_DPOT_POSITION_MIN) ? BOOST_DPOT_POSITION_MIN : position;
    position = (position > BOOST_DPOT_POSITION_MAX) ? BOOST_DPOT_POSITION_MAX : position;
    return position;
}

uint8_t boost_dpot_read_position(boost_dpot_t* dpot) {
    uint8_t wr, rr;
    wr = i2c_write_blocking_until(dpot->port, dpot->addr, &dpot->reg,
                                                1, true,  make_timeout_time_ms(500));
    rr = i2c_read_blocking_until(dpot->port, dpot->addr, &dpot->position,
                                                1, false, make_timeout_time_ms(500));
    dpot->write_status = wr;
    dpot->read_status  = rr;
    return dpot->position;
}

void boost_dpot_write_position(boost_dpot_t* dpot, uint8_t position) {
    uint8_t pos          = boost_dpot_clamp_position(position);
    uint8_t data_pkt[2]  = { dpot->reg, pos };
    dpot->write_status   = i2c_write_blocking_until(dpot->port, dpot->addr, data_pkt,
            2, false, make_timeout_time_ms(100));
    dpot->position       = pos;
}

float boost_dpot_get_voltage(const boost_dpot_t* dpot, const boost_cal_table_t* tbl) {
    if (!dpot || !tbl || !tbl->calibrated || dpot->position >= tbl->count) return 0.0f;
    return tbl->entries[dpot->position].voltage;
}

boost_cal_status_t boost_cal_build(boost_dpot_t* dpot, boost_cal_table_t* tbl,
                                   int adc_samples, uint32_t settle_ms)
{
    if (!dpot || !tbl || adc_samples < 1) return BOOST_CAL_INVALID_INPUT;

    tbl->count      = 0;
    tbl->calibrated = false;

    for (uint8_t pos = BOOST_DPOT_POSITION_MIN; pos <= BOOST_DPOT_POSITION_MAX; pos++) {
        boost_dpot_write_position(dpot, pos);
        sleep_ms(settle_ms);
        tbl->entries[tbl->count].voltage = sensors_read_output_voltage_averaged(adc_samples);
        tbl->entries[tbl->count].wiper   = pos;
        tbl->count++;
    }

    boost_dpot_write_position(dpot, BOOST_DPOT_POSITION_MIN);
    tbl->calibrated = true;
    return BOOST_CAL_OK;
}

uint8_t boost_cal_lookup_wiper(const boost_cal_table_t* tbl, float target_v) {
    /* searches cal table for nearest voltage value and returns the associated wiper pos. */
    if (!tbl || !tbl->calibrated || tbl->count == 0) return BOOST_DPOT_POSITION_MIN;

    int lo = 0, hi = (int)tbl->count - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (tbl->entries[mid].voltage < target_v)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo > 0) {
        float diff_below = target_v - tbl->entries[lo - 1].voltage;
        float diff_above = tbl->entries[lo].voltage - target_v;
        if (diff_below < diff_above) lo--;
    }

    return tbl->entries[lo].wiper;
}

void boost_set_voltage(boost_dpot_t* dpot, const boost_cal_table_t* tbl,
                       float target_v, uint32_t ramp_step_ms)
{
    if (!dpot || !tbl || !tbl->calibrated) return;

    uint8_t target  = boost_cal_lookup_wiper(tbl, target_v);
    uint8_t current = dpot->position;

    while (current != target) {
        current += (current < target) ? 1 : -1;
        boost_dpot_write_position(dpot, current);
        sleep_ms(ramp_step_ms);
    }
}
