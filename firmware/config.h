/* config.h
 *
 * Pin assignments, hardware constants, and safe-operating limits for the
 * EPL Wire EDM Power Supply firmware. No runtime state belongs here.
 */

#ifndef EPL_WPS_CONFIG_H
#define EPL_WPS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*=============================================================================
 * Pin assignments
 *===========================================================================*/
/* Digital I/O */
static const int EDM_ENABLE_PIN         = 2;    /* Digital input from motion controller */
static const int EDM_FEEDBACK_PIN       = 3;    /* PWM output to motion controller (power ratio) */
static const int OUTPUT_OVERCURRENT_PIN = 12;   /* Active-LOW comparator from output current sensor */
static const int BOOST_PGOOD_PIN        = 18;   /* Active-LOW power-good from boost-converter module */
static const int PMM_FAULT_PIN          = 20;   /* Active-LOW fault from power-management module */
static const int PMM_DIAG_EN_PIN        = 21;   /* Enables PMM fault reporting */
static const int PMM_ENABLE_PIN         = 22;   /* PMM high-side switch enable */
static const int STATUS_LED_PIN         = 25;   /* Onboard LED */

/* Output-stage PWM.
 * SW_HIGH_VOLTAGE_PHASE shares its PWM slice with OUTPUT_OVERCURRENT_SET;
 * SW_ENABLE shares with SW_HIGH_CURRENT_PHASE. P-channel switches invert
 * polarity so HIGH = OFF. */
static const int SW_HIGH_VOLTAGE_PHASE_PIN  = 8;   /* PWM 4A, P-channel (inverted) */
static const int OUTPUT_OVERCURRENT_SET_PIN = 9;   /* PWM 4B */
static const int SW_ENABLE_PIN              = 10;  /* PWM 5A, N-channel */
static const int SW_HIGH_CURRENT_PHASE_PIN  = 11;  /* PWM 5B, P-channel (inverted) */

/* Analog inputs */
static const int PMM_ISENSE_PIN    = 26;   /* PMM current sensor (200 mV/A); ADC channel 0 */
static const int OUTPUT_VSENSE_PIN = 27;   /* Output voltage via 99.6:1 divider; ADC channel 1 */
static const int OUTPUT_ISENSE_PIN = 28;   /* Output current sensor TMCS1133 (25 mV/A); ADC channel 2 */

/* I2C (boost-converter DPOT) */
static const int I2C_SDA_PIN = 16;
static const int I2C_SCL_PIN = 17;

/*=============================================================================
 * Hardware constants
 *===========================================================================*/
#define I2C_PORT             i2c0
#define DPOT_ADDR            0x3E
#define DPOT_REG             0x00
#define I2C_BAUD_RATE_HZ     10000

/* Array-size constants must be #define, not static const, for use in C
 * array declarators and struct definitions. */
#define DEVICE_CURRENT_BUFFER_SIZE  100
#define BOOST_DPOT_POSITION_MAX     110
#define MAX_CMD_PARAMS              4

static const int      ADC_RESOLUTION_BITS = 12;
static const uint32_t PWM_BASE_CLOCK_FREQ = 133000000;  /* 133 MHz */

static const uint16_t EDM_FEEDBACK_PWM_CLOCK_DIVIDER = 10;
static const uint32_t EDM_FEEDBACK_PWM_FREQ_HZ       = 1000;

/*=============================================================================
 * Sensor scaling
 *===========================================================================*/
static const float ADC_TO_VOLTS                = 3.3f / 4095.0f;
static const float PMM_ISENSE_V_PER_AMP        = 0.200f;      /* PMM current sensor */
static const float ISENSE_OUTPUT_V_PER_AMP     = 0.025f;      /* TMCS1133C1A */
static const int   ISENSE_OUTPUT_AMPS_PER_VOLT = 40;
static const float OUTPUT_VOLTAGE_SCALE        = 0.08090972f; /* (3.3/4095) * 100.4 */

static const int PMM_CALIBRATION_SAMPLES    = 100;
static const int PMM_CALIBRATION_DELAY_MS   = 1;
static const int OUTPUT_CURRENT_SENSOR_CALIBRATION_SAMPLES  = 100;
static const int OUTPUT_CURRENT_SENSOR_CALIBRATION_DELAY_MS = 1;

/*=============================================================================
 * Safe-operating limits
 *===========================================================================*/
static const float MAX_SAFE_INPUT_CURRENT         = 7.3f;   /* A */
static const float MAX_INPUT_POWER_SETPOINT_WATTS = 75.0f;  /* W */
static const float INPUT_SUPPLY_VOLTAGE           = 48.0f;  /* V (nominal) */

static const int MIN_HIGH_VOLTAGE_PHASE_VOLTS = 64;
static const int MAX_HIGH_VOLTAGE_PHASE_VOLTS = 100;

static const int BOOST_DPOT_POSITION_MIN         = 0;
static const int BOOST_CONVERTER_RAMP_SPACING_MS = 10;

static const float MIN_DUTY_CYCLE             = 0.01f;
static const float MAX_DUTY_CYCLE             = 0.12f;
static const float MIN_MACHINING_FREQUENCY_HZ = 5000.0f;
static const float MAX_MACHINING_FREQUENCY_HZ = 10000.0f;
static const float MIN_ON_TIME_MICROS         = 1.000f;
static const float MAX_ON_TIME_MICROS         = 25.000f;
static const float MIN_OFF_TIME_MICROS        = 88.000f;
static const float MIN_INIT_VOLTAGE           = 64.0f;  /* MIN_HIGH_VOLTAGE_PHASE_VOLTS */
static const float MAX_INIT_VOLTAGE           = 100.0f; /* MAX_HIGH_VOLTAGE_PHASE_VOLTS */

static const int DEFAULT_OUTPUT_CURRENT_THRESHOLD_A = 8;

/* EDM iso-frequency: success-rate calculation window and pulse-skip threshold. */
static const int    DISCHARGES_PER_CALC_INTERVAL         = 10;
static const double MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD = 0.800;

/* EDM Iso-frequency mode: HV pulse on-time and PWM phase offset to prevent
 * shoot-through between SW_HIGH_VOLTAGE_PHASE and SW_ENABLE. */
static const int    EDM_ISOFREQ_HV_PULSE_ON_TIME_US = 1;
static const double EDM_ISOFREQ_HV_PWM_OFFSET       = 0.25;

/*=============================================================================
 * Timing & feature flags
 *===========================================================================*/
static const int  PMM_INRUSH_DELAY_MS      = 500;
static const int  PERIPH_MGMT_INTERVAL_MS  = 10;
static const int  TELEMETRY_INTERVAL_MS    = 1000;

static const bool PERIODIC_TELEMETRY_ENABLED          = true;
static const bool ALLOW_DISCHARGE_SUCCESS_RATE_PULSE_SKIP = true;
static const bool ALLOW_POWER_SETPOINT_PULSE_SKIP     = true;

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_CONFIG_H */
