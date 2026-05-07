/* pmm.h
 *
 * Power-management module driver. The PMM is a separate board carrying
 * a 48 V high-side switch with built-in inrush limiting, overcurrent
 * latch-off, and a diagnostic fault output.
 *
 * Three GPIOs:
 *   - PMM_FAULT_PIN     (input,  active-LOW): fault asserted by PMM
 *   - PMM_DIAG_EN_PIN   (output, active-HIGH): enables fault reporting
 *   - PMM_ENABLE_PIN    (output, active-HIGH): closes the high-side switch
 *
 * Module is a singleton (no state — peripheral state lives in the GPIOs).
 * IRQ wiring for PMM_FAULT_PIN is owned by the fault module / main.c
 * dispatcher, not here; pmm only exposes a polled fault read.
 */

#ifndef EPL_WPS_PMM_H
#define EPL_WPS_PMM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO directions; deassert PMM_ENABLE_PIN; assert PMM_DIAG_EN_PIN so
 * fault reporting is live before power is ever applied. */
void pmm_init(void);

/* Close the high-side switch and block for PMM_INRUSH_DELAY_MS while
 * downstream capacitors charge through the PMM's inrush limiter. */
void pmm_enable_and_wait(void);

/* Open the high-side switch. Does not wait. */
void pmm_disable(void);

/* True when PMM_FAULT_PIN reads LOW (fault asserted). */
bool pmm_is_fault_active(void);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_PMM_H */
