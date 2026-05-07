/* fault.h
 *
 * Fault state machine for the WPS firmware. Three sources:
 *
 *   - PMM_FAULT_PIN    (active-LOW, falling edge — recoverable, brief pause)
 *   - BOOST_PGOOD_PIN  (active-LOW, falling edge — recoverable, spin until
 *                       the pin returns HIGH)
 *   - in-line trips    (POWER_OUT_OF_RANGE_FAULT,
 *                       HIGH_VOLTAGE_PHASE_SETUP_FAULT — non-recoverable;
 *                       operator must issue RESET_DEVICE)
 *
 * Singleton — file-static state in fault.c. Sync between ISR and main
 * loop is via a single volatile flag.
 *
 * GPIO IRQ wiring: only one system-wide GPIO callback exists on the
 * RP2040. main.c owns that callback and invokes fault_pmm_isr() and
 * fault_boost_pgood_isr() based on the firing pin. fault_attach_irqs()
 * arms the falling-edge conditions but does NOT register the system
 * callback.
 *
 * fault_handle() drives STATUS_LED_PIN during recovery; main.c is
 * responsible for the initial pin configuration.
 */

#ifndef EPL_WPS_FAULT_H
#define EPL_WPS_FAULT_H

#include <stdbool.h>

#include "types.h"
#include "ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Lifecycle ------------------------------------------------------- */

/* Reset state to no-fault and configure BOOST_PGOOD_PIN as a digital
 * input. PMM_FAULT_PIN is owned by pmm_init() and is not touched here. */
void fault_init(void);

/* Arm falling-edge IRQ conditions on PMM_FAULT_PIN and BOOST_PGOOD_PIN.
 * The system-wide GPIO callback must be registered separately in main.c. */
void fault_attach_irqs(void);

/* --- State accessors ------------------------------------------------- */

bool          fault_pending(void);
fault_type_t  fault_active_type(void);
const char   *fault_type_name(fault_type_t type);

/* --- Trip and recover ----------------------------------------------- */

/* Latch a fault. ISR-safe. Immediately disables the output stage so the
 * device is in a safe state regardless of when fault_handle() runs. */
void fault_trip(fault_type_t type);

/* Main-loop recovery dispatcher. Prints fault, performs the
 * fault-type-specific recovery wait, and clears the pending flag.
 *
 * cmd_poll_fn is invoked repeatedly during the spin so operator commands
 * (RESET_DEVICE, SEND_TELEMETRY, etc.) remain serviceable. Pass cmd_poll.
 * The function pointer (instead of a direct cmd.h include) keeps the
 * include graph cycle-free. NULL is allowed but disables operator
 * interaction for the duration of the recovery wait. */
void fault_handle(main_ctx_t *ctx, bool (*cmd_poll_fn)(main_ctx_t *));

/* --- ISR entry points ----------------------------------------------- */

/* GPIO dispatcher hook. Call ONLY from the system-wide GPIO IRQ callback
 * in main.c when PMM_FAULT_PIN drops. */
void fault_pmm_isr(void);

/* GPIO dispatcher hook. Call ONLY from the system-wide GPIO IRQ callback
 * in main.c when BOOST_PGOOD_PIN drops. */
void fault_boost_pgood_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_FAULT_H */
