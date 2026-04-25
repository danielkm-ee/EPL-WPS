/* telemetry.h
 *
 * Serial command interface and periodic-status reporting. Owns the
 * Serial.begin() lifecycle, parses inbound commands, and prints the
 * SEND_TELEMETRY block. Reads device-level state (mode, parameters,
 * discharge stats) via externs declared in firmware.ino.
 */

#ifndef EPL_WPS_TELEMETRY_H
#define EPL_WPS_TELEMETRY_H

#include <Arduino.h>

extern const String SOFTWARE_VERSION;

// Open Serial at 115200 baud and block briefly for the host to attach.
void telemetry_setup_serial(void);

// Print the multi-line status block (firmware version, state, fault,
// input current/power, mode-specific stats).
void telemetry_send(void);

// Parse one newline-terminated command and dispatch. Trims leading and
// trailing whitespace. Unknown commands print a help summary.
void telemetry_process_command(String command);

#endif // EPL_WPS_TELEMETRY_H
