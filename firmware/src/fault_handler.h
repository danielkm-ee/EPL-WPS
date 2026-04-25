/* fault_handler.h
 *
 * Attaches fault-source ISRs and dispatches fault handling from the main
 * loop. ISRs keep it thin: acknowledge the IRQ, disable the output stage,
 * mark the fault type, and exit. Everything else — LED, telemetry, recovery
 * wait — happens in fault_handle() called from loop().
 */

#ifndef EPL_WPS_FAULT_HANDLER_H
#define EPL_WPS_FAULT_HANDLER_H

#include "types.h"

void fault_attach_interrupts(void);

// Set from anywhere (ISR or main-loop) to request a fault transition.
// Safe to call before the main loop runs: the loop will pick it up.
void fault_trip(FaultStateType type);

bool fault_pending(void);
FaultStateType fault_current_type(void);

// Blocking handler invoked from loop() when a fault is pending. Drives the
// status LED, prints diagnostics, waits for fault clear (if recoverable),
// and returns once the device is safe to resume.
void fault_handle(void);

const char *fault_type_name(FaultStateType type);

#endif // EPL_WPS_FAULT_HANDLER_H
