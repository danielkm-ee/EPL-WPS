/* main.c
 *
 * EPL WPS firmware entry point. Owns the runtime context, the boost
 * converter handle, and the system-wide GPIO IRQ dispatcher. All other
 * state lives file-static inside its respective module.
 *
 * Boot sequence:
 *   ctx_init -> cmd_init -> output_init -> pmm_init -> sensors_init ->
 *   boost create -> calibrate input current (PMM off) ->
 *   pmm_enable_and_wait -> calibrate output current ->
 *   register GPIO dispatcher -> fault_init / fault_attach_irqs ->
 *   output_attach_isr -> HV-only output stage -> boost_cal_build ->
 *   MAX-safe HV check -> output_stage_disable -> IDLE.
 *
 * Main loop, in order:
 *   1. Service any pending fault.
 *   2. cmd_poll (every iteration).
 *   3. Periph tick every PERIPH_MGMT_INTERVAL_MS: PMM sample, power-out-
 *      of-range check, EDM_ENABLE_PIN -> state, feedback-duty update.
 *   4. State dispatch: OPERATING preps mode lazily then runs the per-
 *      tick mode body; IDLE disables output and sleeps 10 ms.
 *   5. cmd_tick (periodic telemetry).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "config.h"
#include "src/types.h"
#include "src/ctx.h"
#include "src/cmd.h"
#include "src/sensors.h"
#include "src/pmm.h"
#include "src/boost.h"
#include "src/output.h"
#include "src/fault.h"

/* --- File-static state ------------------------------------------------ */

static main_ctx_t        g_ctx;
static boost_dpot_t     *g_boost_dpot;
static boost_cal_table_t g_boost_cal;

/* --- GPIO IRQ dispatcher --------------------------------------------- */

/* Single system-wide GPIO callback. The RP2040 SDK only allows one;
 * each module exposes a thin _isr() entry that this dispatcher calls
 * based on the firing pin. */
static void gpio_irq_dispatch(uint gpio, uint32_t events)
{
    (void)events;
    if      (gpio == PMM_FAULT_PIN)          fault_pmm_isr();
    else if (gpio == BOOST_PGOOD_PIN)        fault_boost_pgood_isr();
    else if (gpio == OUTPUT_OVERCURRENT_PIN) output_overcurrent_isr();
}

/* --- Boot helpers ---------------------------------------------------- */

static void status_led_init(void)
{
    gpio_init(STATUS_LED_PIN);
    gpio_put(STATUS_LED_PIN, false);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
}

static void edm_enable_pin_init(void)
{
    gpio_init(EDM_ENABLE_PIN);
    gpio_set_dir(EDM_ENABLE_PIN, GPIO_IN);
}

/* Scan the cal table; trip a non-recoverable HV-setup fault if any
 * calibrated DPOT position produced a voltage above the safe ceiling.
 * Trips on an empty / failed cal table too (calibrated == false). */
static void verify_boost_cal_safe(void)
{
    if (!g_boost_cal.calibrated) {
        printf("ERROR: boost cal table not built\n");
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
        return;
    }
    float max_v = 0.0f;
    for (uint8_t i = 0; i < g_boost_cal.count; i++) {
        if (g_boost_cal.entries[i].voltage > max_v) {
            max_v = g_boost_cal.entries[i].voltage;
        }
    }
    if (max_v > (float)MAX_HIGH_VOLTAGE_PHASE_VOLTS) {
        printf("ERROR: boost cal max voltage %.2f V exceeds safe ceiling %d V\n",
               (double)max_v, MAX_HIGH_VOLTAGE_PHASE_VOLTS);
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
    }
    if (max_v < (float)MIN_HIGH_VOLTAGE_PHASE_VOLTS) {
        printf("ERROR: boost cal max voltage %.2f V lower than minimum %d V\n",
               (double)max_v, MIN_HIGH_VOLTAGE_PHASE_VOLTS);
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
    }
}

/* --- Mode preparation (OPERATING entry) ----------------------------- */

static void prepare_isofreq_mode(void)
{
    boost_set_voltage(g_boost_dpot, &g_boost_cal,
                      g_ctx.params.init_voltage,
                      (uint32_t)BOOST_CONVERTER_RAMP_SPACING_MS);
    if (!output_setup_isofreq(&g_ctx, &g_boost_cal)) {
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
    }
}

static void prepare_edge_mode(void)
{
    boost_set_voltage(g_boost_dpot, &g_boost_cal,
                      (float)MIN_HIGH_VOLTAGE_PHASE_VOLTS,
                      (uint32_t)BOOST_CONVERTER_RAMP_SPACING_MS);
    if (!output_setup_edge(&g_boost_cal)) {
        fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT);
    }
}

/* --- Periph tick (every PERIPH_MGMT_INTERVAL_MS) -------------------- */

static void periph_tick(void)
{
    sensors_sample_input_current();
    if (sensors_avg_input_current_amps() > MAX_SAFE_INPUT_CURRENT) {
        fault_trip(POWER_OUT_OF_RANGE_FAULT);
        return;
    }

    ctx_set_state(&g_ctx,
                  gpio_get(EDM_ENABLE_PIN) ? OPERATING : IDLE);

    if (g_ctx.state == OPERATING) {
        float power_w = sensors_avg_input_current_amps() * INPUT_SUPPLY_VOLTAGE;
        float ratio   = power_w / MAX_INPUT_POWER_SETPOINT_WATTS;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        /* Active-low feedback: 0.0 = full power request, 1.0 = idle. */
        output_feedback_set_duty(1.0f - ratio);
    } else {
        output_feedback_set_duty(0.0f);
    }
}

/* --- Entry point ----------------------------------------------------- */

int main(void)
{
    /* 1. Runtime context. State starts as STARTUP. */
    ctx_init(&g_ctx);

    /* 2. USB-CDC up first so the rest of boot can log. */
    cmd_init();
    printf("Software Version: %s\n", "rev0");

    /* 3. STATUS LED off until boot completes. */
    status_led_init();

    /* 4. EDM_ENABLE input. */
    edm_enable_pin_init();

    /* 5. Output stage: cache PWM slices, configure feedback PWM,
     *    drive switches into safe-OFF SIO state. */
    output_init();

    /* 6. PMM GPIOs: ENABLE deasserted, DIAG_EN asserted, FAULT as input.
     *    Must precede sensors_calibrate_input_current — calibrating with
     *    the high-side switch off captures the true zero-current offset. */
    pmm_init();

    /* 7. ADC peripheral and three sensor pins. */
    sensors_init();

    /* 8. Boost DPOT handle + I2C wiring. Cal table starts uncalibrated. */
    g_boost_dpot = boost_dpot_create(I2C_PORT, DPOT_ADDR, DPOT_REG);
    boost_setup_i2c(g_boost_dpot, I2C_SDA_PIN, I2C_SCL_PIN, I2C_BAUD_RATE_HZ);
    cmd_set_boost_ctx(g_boost_dpot, &g_boost_cal);

    /* 9. Calibrate input current with the high-side switch still off. */
    printf("Calibrating PMM input-current zero offset...\n");
    sensors_calibrate_input_current();

    /* 10. Bring up the PMM and wait for inrush to settle. */
    printf("Enabling PMM...\n");
    pmm_enable_and_wait();

    /* 11. Output current zero-cal (HV stage idle, no discharge yet). */
    printf("Calibrating output-current zero offset...\n");
    sensors_calibrate_output_current();

    /* 12. Register the system-wide GPIO callback once. Per-pin enables
     *     come from fault_attach_irqs and output_attach_isr; they use
     *     gpio_set_irq_enabled (no callback arg) which attaches to the
     *     dispatcher we just registered. */
    gpio_set_irq_callback(gpio_irq_dispatch);
    irq_set_enabled(IO_IRQ_BANK0, true);

    /* 13. Fault module: state reset, BOOST_PGOOD pin direction, edge
     *     IRQs armed. */
    fault_init();
    fault_attach_irqs();

    /* 14. Output ISR binding + OUTPUT_OVERCURRENT edge IRQ. */
    output_attach_isr(&g_ctx);

    /* 15. Hold HV stage active so the boost can be calibrated. */
    printf("Holding HV phase active for boost calibration...\n");
    output_stage_setup(0.0f, 10000,
                       (float)DEFAULT_OUTPUT_CURRENT_THRESHOLD_A,
                       false, true,
                       (float)EDM_ISOFREQ_HV_PULSE_ON_TIME_US,
                       (float)EDM_ISOFREQ_HV_PWM_OFFSET);

    /* 16. Sweep DPOT positions and record the wiper-to-voltage table. */
    printf("Building boost-converter DPOT voltage table...\n");
    boost_cal_status_t cal_status =
        boost_cal_build(g_boost_dpot, &g_boost_cal, 8, 20);
    if (cal_status != BOOST_CAL_OK) {
        printf("ERROR: boost_cal_build failed (status=%d)\n", (int)cal_status);
    }

    /* 17. Trip if the boost can produce voltages above the safe ceiling
     *     or if the cal table didn't get built. Non-recoverable: the
     *     main loop's first iteration will see fault_pending and spin
     *     in fault_handle until the operator issues RESET_DEVICE. */
    verify_boost_cal_safe();

    /* 18. Drop output stage now that calibration is done. */
    output_stage_disable();

    /* 19. Boot complete (or boot failed). */
    if (fault_pending()) {
        printf("ERROR: setup failed; fault pending, awaiting recovery\n");
    } else {
        gpio_put(STATUS_LED_PIN, true);
        printf("OK: Setup complete\n");
        ctx_set_state(&g_ctx, IDLE);
    }

    /* --- Main loop --------------------------------------------------- */

    while (1) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        /* 1. Pending fault: hand control to the fault handler. */
        if (fault_pending()) {
            ctx_set_state(&g_ctx, FAULT);
            fault_handle(&g_ctx, cmd_poll);
            ctx_reset_discharge_stats(&g_ctx);
            ctx_set_state(&g_ctx, IDLE);
            continue;
        }

        /* 2. Operator commands every iteration. */
        cmd_poll(&g_ctx);

        /* 3. Periph tick at fixed cadence. */
        if (now_ms - g_ctx.last_periph_tick_ms
            >= (uint32_t)PERIPH_MGMT_INTERVAL_MS) {
            periph_tick();
            g_ctx.last_periph_tick_ms = now_ms;
        }

        /* 4. State dispatch. */
        switch (g_ctx.state) {
        case OPERATING:
            if (!g_ctx.mode_prep_done) {
                if (g_ctx.mode == EDM_ISOFREQUENCY_MODE) prepare_isofreq_mode();
                else                                     prepare_edge_mode();
                /* Don't latch mode_prep_done if prep tripped a fault. */
                if (!fault_pending()) g_ctx.mode_prep_done = true;
            }
            if (g_ctx.mode_prep_done) {
                if (g_ctx.mode == EDM_ISOFREQUENCY_MODE) output_run_isofreq(&g_ctx);
                else                                     output_run_edge(&g_ctx);
            }
            break;

        case IDLE:
            output_stage_disable();
            g_ctx.mode_prep_done           = false;
            g_ctx.discharges_since_op_start = 0;
            sleep_ms(10);
            break;

        case STARTUP:
        case FAULT:
            /* STARTUP only persists if boot tripped a fault before we
             * reached IDLE; FAULT is set above and exited via continue. */
            break;
        }

        /* 5. Periodic telemetry. */
        cmd_tick(&g_ctx);
    }

    /* Unreachable. */
    return 0;
}
