/* cmd.h
 *
 * USB-CDC command interface for the WPS firmware. Owns the stdio_usb
 * lifecycle, parses newline-terminated commands from the host, and
 * dispatches them via a static command table. Also owns periodic
 * telemetry transmission when ctx->periodic_telemetry_enabled is true.
 *
 * Singleton — file-static state in cmd.c. The boost-converter
 * dependencies (DPOT + cal table) are injected once via
 * cmd_set_boost_ctx() before the first cmd_poll().
 *
 * Dispatch rules:
 *   - Command names are matched exactly (no startsWith / prefix match).
 *   - Each entry declares [argc_min, argc_max]; the dispatcher rejects
 *     malformed argument counts before invoking the handler.
 *   - Unknown commands print "ERROR: Unknown command 'X'" followed by
 *     the full HELP listing.
 *   - All status / error messages use "OK: ..." or "ERROR: ..." prefixes.
 *
 * The dispatcher is wired so fault.c can pass &cmd_poll into
 * fault_handle() without creating a circular include between fault.h
 * and cmd.h: fault stays unaware of the command table.
 */

#ifndef EPL_WPS_CMD_H
#define EPL_WPS_CMD_H

#include <stdbool.h>

#include "ctx.h"
#include "boost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Lifecycle ------------------------------------------------------- */

/* stdio_usb_init() and a brief blocking wait for USB-CDC enumeration so
 * early prints land in a connected terminal. Call once at boot before
 * the main loop. */
void cmd_init(void);

/* One-shot wiring of the boost-converter dependencies. Must be called
 * before any DPOT / voltage-table command can succeed. The pointers
 * must outlive the cmd module (typically file-static in main.c). */
void cmd_set_boost_ctx(boost_dpot_t *dpot, boost_cal_table_t *cal);

/* --- Per-iteration entry points ------------------------------------- */

/* Drain pending stdio bytes. Each newline triggers tokenization and
 * dispatch of one command. Non-blocking. Returns true iff at least one
 * complete command line was processed during this call.
 *
 * Designed to be passed to fault_handle() as the cmd_poll_fn callback
 * so operator commands remain serviceable during recovery waits. */
bool cmd_poll(main_ctx_t *ctx);

/* Periodic telemetry tick. Emits a status block every
 * TELEMETRY_INTERVAL_MS when ctx->periodic_telemetry_enabled is true.
 * No-op otherwise. */
void cmd_tick(main_ctx_t *ctx);

/* --- Direct invocation ---------------------------------------------- */

/* Print the multi-line status block (firmware version, state, fault,
 * input current/power, mode-specific stats). Called from cmd_tick and
 * from the SEND_TELEMETRY handler. */
void cmd_send_telemetry(const main_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EPL_WPS_CMD_H */
