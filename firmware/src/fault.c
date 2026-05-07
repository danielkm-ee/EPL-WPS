/* fault.c
 *
 * Fault state machine: ISRs latch a flag and force the output stage
 * safe; fault_handle() in the main loop drives the LED, prints
 * diagnostics, and waits for recovery while servicing commands.
 *
 * GPIO IRQ acknowledgement is handled by the SDK's default dispatcher
 * for edge-triggered events, so the ISRs here do not need to ack.
 */

#include <stdio.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "fault.h"
#include "output.h"
#include "ctx.h"
#include "../config.h"

/* --- File-static state ----------------------------------------------- */

static volatile bool         g_pending = false;
static volatile fault_type_t g_active  = PMM_FAULT_TYPE;

/* --- Internal helpers ------------------------------------------------ */

/* BOOST_PGOOD_PIN is active-LOW: HIGH = power good, fault cleared. */
static bool fault_boost_recovered(void)
{
    return gpio_get(BOOST_PGOOD_PIN) != 0;
}

/* Spin until predicate returns true (or forever if predicate is NULL),
 * polling the operator command interface. STATUS_LED_PIN is dropped
 * while waiting and restored on exit. */
static void fault_spin_until(bool (*recovered)(void),
                             main_ctx_t *ctx,
                             bool (*cmd_poll_fn)(main_ctx_t *))
{
    gpio_put(STATUS_LED_PIN, false);
    while (recovered == NULL || !recovered()) {
        if (cmd_poll_fn) cmd_poll_fn(ctx);
    }
    gpio_put(STATUS_LED_PIN, true);
}

/* --- Lifecycle -------------------------------------------------------- */

void fault_init(void)
{
    g_pending = false;
    g_active  = PMM_FAULT_TYPE;

    /* External pull-up on the boost-converter module holds this HIGH
     * when power is good. PMM_FAULT_PIN is configured by pmm_init. */
    gpio_init(BOOST_PGOOD_PIN);
    gpio_set_dir(BOOST_PGOOD_PIN, GPIO_IN);
}

void fault_attach_irqs(void)
{
    gpio_set_irq_enabled(PMM_FAULT_PIN,   GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BOOST_PGOOD_PIN, GPIO_IRQ_EDGE_FALL, true);
}

/* --- State accessors ------------------------------------------------- */

bool fault_pending(void)
{
    return g_pending;
}

fault_type_t fault_active_type(void)
{
    return g_active;
}

const char *fault_type_name(fault_type_t type)
{
    switch (type) {
    case PMM_FAULT_TYPE:                 return "PMM_FAULT";
    case BOOST_CONVERTER_PGOOD_FAULT:    return "BOOST_CONVERTER_PGOOD_FAULT";
    case POWER_OUT_OF_RANGE_FAULT:       return "POWER_OUT_OF_RANGE_FAULT";
    case HIGH_VOLTAGE_PHASE_SETUP_FAULT: return "HIGH_VOLTAGE_PHASE_SETUP_FAULT";
    }
    return "UNKNOWN_FAULT";
}

/* --- Trip and recover ----------------------------------------------- */

void fault_trip(fault_type_t type)
{
    /* Disable from the trip path so the device is safe even if
     * fault_handle() runs many ms later. */
    output_stage_disable();
    g_active  = type;
    g_pending = true;
}

void fault_handle(main_ctx_t *ctx, bool (*cmd_poll_fn)(main_ctx_t *))
{
    if (!g_pending) return;

    /* Defensive re-disable in case fault_trip was reached from a path
     * that didn't already disable (no-op if already safe). */
    output_stage_disable();
    output_feedback_set_duty(0.0f);

    printf("FAULT: %s\n", fault_type_name(g_active));

    switch (g_active) {
    case PMM_FAULT_TYPE:
        /* Recoverable. Brief pause for the PMM's internal latch-off to
         * settle, then resume. Matches stale firmware behavior. */
        gpio_put(STATUS_LED_PIN, false);
        sleep_ms(500);
        gpio_put(STATUS_LED_PIN, true);
        printf("PMM Fault Cleared\n");
        break;

    case BOOST_CONVERTER_PGOOD_FAULT:
        fault_spin_until(fault_boost_recovered, ctx, cmd_poll_fn);
        printf("High Voltage Phase Fault Cleared\n");
        break;

    case POWER_OUT_OF_RANGE_FAULT:
    case HIGH_VOLTAGE_PHASE_SETUP_FAULT:
        /* Non-recoverable: spin in the command interface until operator
         * issues RESET_DEVICE. */
        fault_spin_until(NULL, ctx, cmd_poll_fn);
        break;
    }

    g_pending = false;
}

/* --- ISR entry points ----------------------------------------------- */

void fault_pmm_isr(void)
{
    fault_trip(PMM_FAULT_TYPE);
}

void fault_boost_pgood_isr(void)
{
    fault_trip(BOOST_CONVERTER_PGOOD_FAULT);
}
