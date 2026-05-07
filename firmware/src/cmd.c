/* cmd.c
 *
 * USB-CDC command parser, dispatcher, and periodic telemetry tick. See
 * cmd.h for module-level architecture notes.
 */

/* strtok_r is POSIX, hidden under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "cmd.h"
#include "ctx.h"
#include "types.h"
#include "output.h"
#include "fault.h"
#include "sensors.h"
#include "boost.h"
#include "../config.h"

#define SOFTWARE_VERSION    "1.0-beta"
#define CMD_LINE_BUFFER_SZ  128
#define CMD_MAX_TOKENS      (MAX_CMD_PARAMS + 1)   /* name + up to MAX_CMD_PARAMS args */
#define CMD_USB_ENUM_MS     1000
#define CMD_REBOOT_FLUSH_MS 50

/* --- File-static state ------------------------------------------------ */

static char   g_line[CMD_LINE_BUFFER_SZ];
static size_t g_line_idx;
static bool   g_line_overflow;

static boost_dpot_t      *g_dpot;
static boost_cal_table_t *g_cal;

/* --- Command table type (private) ----------------------------------- */

typedef void (*cmd_handler_fn)(int argc, char **argv, main_ctx_t *ctx);

typedef struct {
    const char     *name;       /* exact-match command keyword */
    int             argc_min;   /* required arg count, lower bound */
    int             argc_max;   /* required arg count, upper bound */
    cmd_handler_fn  handler;
    const char     *usage;      /* one-line syntax */
    const char     *desc;       /* one-line description for HELP listing */
} cmd_entry_t;

/* --- Forward declarations ------------------------------------------- */

static const cmd_entry_t *cmd_lookup(const char *name);
static void cmd_print_listing(void);
static void cmd_dispatch(int argc, char **argv, main_ctx_t *ctx);

static void cmd_handle_send_telemetry           (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_set_all_parameters       (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_edge_detection_mode      (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_edm_isofrequency_mode    (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_reset_device             (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_set_dpot                 (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_read_hvp_voltage         (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_update_dpot_voltage_table(int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_set_dpot_from_vtable     (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_set_feedback_duty        (int argc, char **argv, main_ctx_t *ctx);
static void cmd_handle_help                     (int argc, char **argv, main_ctx_t *ctx);

/* --- Command table -------------------------------------------------- */

static const cmd_entry_t g_cmd_table[] = {
    { "SEND_TELEMETRY",            0, 0, cmd_handle_send_telemetry,
      "SEND_TELEMETRY",
      "Print the current device status block." },

    { "SET_ALL_PARAMETERS",        4, 4, cmd_handle_set_all_parameters,
      "SET_ALL_PARAMETERS <discharges> <duty> <freq_hz> <init_v>",
      "Set all isopulse machining parameters at once." },

    { "EDGE_DETECTION_MODE",       0, 0, cmd_handle_edge_detection_mode,
      "EDGE_DETECTION_MODE",
      "Switch the output stage into workpiece-edge probing mode." },

    { "EDM_ISOFREQUENCY_MODE",     0, 0, cmd_handle_edm_isofrequency_mode,
      "EDM_ISOFREQUENCY_MODE",
      "Switch the output stage into EDM iso-frequency machining mode." },

    { "RESET_DEVICE",              0, 0, cmd_handle_reset_device,
      "RESET_DEVICE",
      "Disable the output stage and reboot the MCU." },

    { "SET_DPOT",                  1, 1, cmd_handle_set_dpot,
      "SET_DPOT <position>",
      "Write the boost-converter DPOT wiper position directly." },

    { "READ_HVP_VOLTAGE",          0, 0, cmd_handle_read_hvp_voltage,
      "READ_HVP_VOLTAGE",
      "Print a 10-sample average of the high-voltage-phase output." },

    { "UPDATE_DPOT_VOLTAGE_TABLE", 0, 0, cmd_handle_update_dpot_voltage_table,
      "UPDATE_DPOT_VOLTAGE_TABLE",
      "Sweep the boost DPOT and rebuild the wiper-to-voltage cal table." },

    { "SET_DPOT_FROM_VTABLE",      1, 1, cmd_handle_set_dpot_from_vtable,
      "SET_DPOT_FROM_VTABLE <volts>",
      "Ramp the boost converter to a target voltage via the cal table." },

    { "SET_FEEDBACK_DUTY",         1, 1, cmd_handle_set_feedback_duty,
      "SET_FEEDBACK_DUTY <0.0..1.0>",
      "Override the EDM_FEEDBACK PWM duty (active-low signal to motion ctrl)." },

    { "HELP",                      0, 1, cmd_handle_help,
      "HELP [<command>]",
      "List all commands, or print full usage for one command." },

    { NULL, 0, 0, NULL, NULL, NULL }   /* sentinel */
};

/* --- Lifecycle -------------------------------------------------------- */

void cmd_init(void)
{
    /* stdio_init_all dispatches to whichever backends were enabled in
     * CMake (pico_enable_stdio_usb / _uart). For this project that's
     * USB-CDC only. */
    stdio_init_all();
    sleep_ms(CMD_USB_ENUM_MS);
}

void cmd_set_boost_ctx(boost_dpot_t *dpot, boost_cal_table_t *cal)
{
    g_dpot = dpot;
    g_cal  = cal;
}

/* --- Lookup, dispatch, listing -------------------------------------- */

static const cmd_entry_t *cmd_lookup(const char *name)
{
    for (const cmd_entry_t *e = g_cmd_table; e->name != NULL; e++) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

static void cmd_print_listing(void)
{
    printf("Available commands:\n");
    for (const cmd_entry_t *e = g_cmd_table; e->name != NULL; e++) {
        printf("  %-58s - %s\n", e->usage, e->desc);
    }
}

static void cmd_dispatch(int argc, char **argv, main_ctx_t *ctx)
{
    const cmd_entry_t *e = cmd_lookup(argv[0]);
    if (!e) {
        printf("ERROR: Unknown command '%s'\n", argv[0]);
        cmd_print_listing();
        return;
    }
    int args = argc - 1;
    if (args < e->argc_min || args > e->argc_max) {
        if (e->argc_min == e->argc_max) {
            printf("ERROR: %s expects %d argument(s), got %d\n",
                   e->name, e->argc_min, args);
        } else {
            printf("ERROR: %s expects %d..%d arguments, got %d\n",
                   e->name, e->argc_min, e->argc_max, args);
        }
        printf("Usage: %s\n", e->usage);
        return;
    }
    e->handler(argc, argv, ctx);
}

/* --- Per-iteration entry points -------------------------------------- */

bool cmd_poll(main_ctx_t *ctx)
{
    bool processed = false;
    int  c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (g_line_overflow) {
                printf("ERROR: line too long (max %d bytes)\n",
                       CMD_LINE_BUFFER_SZ - 1);
            } else if (g_line_idx > 0) {
                g_line[g_line_idx] = '\0';
                char *argv[CMD_MAX_TOKENS] = { NULL };
                int   argc     = 0;
                bool  too_many = false;
                char *saveptr  = NULL;
                for (char *tok = strtok_r(g_line, " \t", &saveptr);
                     tok != NULL;
                     tok = strtok_r(NULL, " \t", &saveptr)) {
                    if (argc >= CMD_MAX_TOKENS) {
                        too_many = true;
                        break;
                    }
                    argv[argc++] = tok;
                }
                if (too_many) {
                    printf("ERROR: too many arguments (max %d)\n",
                           MAX_CMD_PARAMS);
                } else if (argc > 0) {
                    cmd_dispatch(argc, argv, ctx);
                    processed = true;
                }
            }
            g_line_idx      = 0;
            g_line_overflow = false;
            continue;
        }
        if (g_line_idx + 1 >= CMD_LINE_BUFFER_SZ) {
            g_line_overflow = true;
            continue;
        }
        g_line[g_line_idx++] = (char)c;
    }
    return processed;
}

void cmd_tick(main_ctx_t *ctx)
{
    if (!ctx->periodic_telemetry_enabled) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - ctx->last_telemetry_tick_ms >= (uint32_t)TELEMETRY_INTERVAL_MS) {
        cmd_send_telemetry(ctx);
        ctx->last_telemetry_tick_ms = now;
    }
}

/* --- Telemetry block ------------------------------------------------- */

static const char *device_state_name(device_state_t s)
{
    switch (s) {
    case STARTUP:   return "STARTUP";
    case FAULT:     return "FAULT";
    case OPERATING: return "OPERATING";
    case IDLE:      return "IDLE";
    }
    return "UNKNOWN";
}

void cmd_send_telemetry(const main_ctx_t *ctx)
{
    printf("FIRMWARE_VERSION %s\n", SOFTWARE_VERSION);
    printf("STATE %s\n", device_state_name(ctx->state));
    if (ctx->state == FAULT) {
        printf("FAULT %s\n", fault_type_name(fault_active_type()));
    } else {
        printf("FAULT NONE\n");
    }

    float in_a = sensors_avg_input_current_amps();
    printf("INPUT_CURRENT %.2f A\n", (double)in_a);
    printf("INPUT_POWER %d W\n", (int)(in_a * INPUT_SUPPLY_VOLTAGE));

    if (ctx->mode == EDGE_DETECTION_MODE) {
        printf("%s\n", ctx->edge_detected
                       ? "EDGE DETECTED"
                       : "NO EDGE DETECTED YET");
        return;
    }

    /* EDM_ISOFREQUENCY_MODE block. */
    printf("AVG_DISCHARGE_CURRENT:%.2f A\n", ctx->avg_discharge_current);
    printf("AVG_DISCHARGE_VOLTAGE:%.2f V\n", ctx->avg_discharge_voltage);
    printf("AVG_OUTPUT_POWER:%d W\n",
           (int)(ctx->avg_discharge_voltage
                 * ctx->avg_discharge_current
                 * (double)ctx->params.duty_cycle
                 * ctx->avg_discharge_success_rate));
    printf("DISCHARGES_SINCE_OPERATION_STARTED:%d\n",
           ctx->discharges_since_op_start);
    printf("AVG_DISCHARGE_SUCCESS_RATE:%d%%\n",
           (int)(ctx->avg_discharge_success_rate * 100.0));
    printf("DISCHARGE_SUCCESS_RATE_THRESHOLD:%d%%\n",
           (int)(MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD * 100.0));
    printf("ISOPULSE PARAMETERS:\n");
    if (ctx->params.discharge_count_target == 0) {
        printf("Infinite Discharges Requested\n");
    } else {
        printf("Discharges Requested:%d\n", ctx->params.discharge_count_target);
    }
    printf("Machining Duty Cycle:%.3f\n",  (double)ctx->params.duty_cycle);
    printf("Machining Frequency:%.1f Hz\n", (double)ctx->params.frequency_hz);
    printf("Initiation Voltage:%.1f V\n",   (double)ctx->params.init_voltage);
    printf("--------------------------------\n");
}

/* --- Parameter validation ------------------------------------------- */

static bool apply_parameters(main_ctx_t *ctx, int discharges, float duty,
                             float frequency, float init_v)
{
    if (discharges < 0) {
        printf("ERROR: Invalid discharges value\n");
        return false;
    }
    if (duty < MIN_DUTY_CYCLE || duty > MAX_DUTY_CYCLE) {
        printf("ERROR: Invalid duty cycle (must be %.3f..%.3f)\n",
               (double)MIN_DUTY_CYCLE, (double)MAX_DUTY_CYCLE);
        return false;
    }
    if (frequency < MIN_MACHINING_FREQUENCY_HZ
        || frequency > MAX_MACHINING_FREQUENCY_HZ) {
        printf("ERROR: Invalid frequency (must be %.0f..%.0f Hz)\n",
               (double)MIN_MACHINING_FREQUENCY_HZ,
               (double)MAX_MACHINING_FREQUENCY_HZ);
        return false;
    }
    if (init_v < MIN_INIT_VOLTAGE || init_v > MAX_INIT_VOLTAGE) {
        printf("ERROR: Invalid initiation voltage (must be %.0f..%.0f V)\n",
               (double)MIN_INIT_VOLTAGE, (double)MAX_INIT_VOLTAGE);
        return false;
    }
    float period_us = 1000000.0f / frequency;
    float on_time   = period_us * duty;
    float off_time  = period_us - on_time;
    if (on_time < MIN_ON_TIME_MICROS || on_time > MAX_ON_TIME_MICROS) {
        printf("ERROR: Calculated on-time (%.2f us) outside valid range\n",
               (double)on_time);
        return false;
    }
    if (off_time < MIN_OFF_TIME_MICROS) {
        printf("ERROR: Calculated off-time (%.2f us) too short\n",
               (double)off_time);
        return false;
    }
    ctx->params.discharge_count_target = discharges;
    ctx->params.duty_cycle             = duty;
    ctx->params.frequency_hz           = frequency;
    ctx->params.init_voltage           = init_v;
    ctx->mode_prep_done                = false;
    return true;
}

/* --- Handlers -------------------------------------------------------- */

static void cmd_handle_send_telemetry(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)argv;
    cmd_send_telemetry(ctx);
}

static void cmd_handle_set_all_parameters(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc;
    int   discharges = atoi(argv[1]);
    float duty       = strtof(argv[2], NULL);
    float frequency  = strtof(argv[3], NULL);
    float init_v     = strtof(argv[4], NULL);
    if (apply_parameters(ctx, discharges, duty, frequency, init_v)) {
        printf("OK: Parameters set\n");
    }
}

static void cmd_handle_edge_detection_mode(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)argv;
    output_stage_disable();
    ctx_set_mode(ctx, EDGE_DETECTION_MODE);
    ctx->mode_prep_done = false;
    printf("OK: EDGE_DETECTION_MODE entered\n");
}

static void cmd_handle_edm_isofrequency_mode(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)argv;
    output_stage_disable();
    ctx_set_mode(ctx, EDM_ISOFREQUENCY_MODE);
    ctx->mode_prep_done = false;
    printf("OK: EDM_ISOFREQUENCY_MODE entered\n");
}

static void cmd_handle_reset_device(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)argv; (void)ctx;
    output_stage_disable();
    printf("OK: Rebooting device\n");
    fflush(stdout);
    /* Brief pause so the reply reaches the host before the watchdog
     * yanks USB. */
    sleep_ms(CMD_REBOOT_FLUSH_MS);
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
}

static void cmd_handle_set_dpot(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)ctx;
    if (!g_dpot) {
        printf("ERROR: boost ctx not bound\n");
        return;
    }
    int pos = atoi(argv[1]);
    if (pos < BOOST_DPOT_POSITION_MIN || pos > BOOST_DPOT_POSITION_MAX) {
        printf("ERROR: position out of range (%d..%d)\n",
               BOOST_DPOT_POSITION_MIN, BOOST_DPOT_POSITION_MAX);
        return;
    }
    boost_dpot_write_position(g_dpot, (uint8_t)pos);
    printf("OK: DPOT set to %d\n", pos);
}

static void cmd_handle_read_hvp_voltage(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)argv; (void)ctx;
    float v = sensors_read_output_voltage_averaged(10);
    printf("HVP_AVG_VOLTAGE %.2f\n", (double)v);
}

static void cmd_handle_update_dpot_voltage_table(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)argv; (void)ctx;
    if (!g_dpot || !g_cal) {
        printf("ERROR: boost ctx not bound\n");
        return;
    }
    boost_cal_status_t s = boost_cal_build(g_dpot, g_cal, 8, 20);
    if (s == BOOST_CAL_OK) {
        printf("OK: DPOT voltage table updated\n");
    } else {
        printf("ERROR: cal_build failed (status=%d)\n", (int)s);
    }
}

static void cmd_handle_set_dpot_from_vtable(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)ctx;
    if (!g_dpot || !g_cal) {
        printf("ERROR: boost ctx not bound\n");
        return;
    }
    if (!g_cal->calibrated) {
        printf("ERROR: cal table not built (run UPDATE_DPOT_VOLTAGE_TABLE first)\n");
        return;
    }
    int target = atoi(argv[1]);
    if (target < MIN_HIGH_VOLTAGE_PHASE_VOLTS
        || target > MAX_HIGH_VOLTAGE_PHASE_VOLTS) {
        printf("ERROR: Target voltage out of range (%d..%d V)\n",
               MIN_HIGH_VOLTAGE_PHASE_VOLTS, MAX_HIGH_VOLTAGE_PHASE_VOLTS);
        return;
    }
    boost_set_voltage(g_dpot, g_cal, (float)target,
                      (uint32_t)BOOST_CONVERTER_RAMP_SPACING_MS);
    printf("OK: Boost ramped to %d V\n", target);
}

static void cmd_handle_set_feedback_duty(int argc, char **argv, main_ctx_t *ctx)
{
    (void)argc; (void)ctx;
    float d = strtof(argv[1], NULL);
    if (d < 0.0f || d > 1.0f) {
        printf("ERROR: Duty cycle must be between 0.0 and 1.0\n");
        return;
    }
    output_feedback_set_duty(d);
    printf("OK: Feedback duty set to %.3f\n", (double)d);
}

static void cmd_handle_help(int argc, char **argv, main_ctx_t *ctx)
{
    (void)ctx;
    if (argc == 1) {
        cmd_print_listing();
        return;
    }
    const cmd_entry_t *e = cmd_lookup(argv[1]);
    if (!e) {
        printf("ERROR: Unknown command '%s'\n", argv[1]);
        return;
    }
    printf("Usage: %s\n", e->usage);
    printf("       %s\n", e->desc);
}
