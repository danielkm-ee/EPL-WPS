/* boost_module.h
 *
 * Driver for the boost-converter module's digital potentiometer (TPL0401B
 * over I2C). Builds and owns a voltage lookup table at startup and
 * translates volt targets into DPOT positions at runtime.
 */

#ifndef EPL_WPS_BOOST_MODULE_H
#define EPL_WPS_BOOST_MODULE_H

#include <stdint.h>

void    boost_setup_i2c(void);

uint8_t boost_dpot_read(void);
void    boost_dpot_write(int position);

// Sweep the DPOT across its range while measuring output voltage to build
// the lookup table. Blocks for roughly 3 s; calls fault handler on failure.
void    boost_build_voltage_table(void);

// Ramp the DPOT to the position whose table-entry best matches target_volts.
// Steps through positions with spacing_ms between writes.
void    boost_set_voltage(int target_volts, int spacing_ms);

#endif // EPL_WPS_BOOST_MODULE_H
