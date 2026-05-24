# EPL WPS Firmware — Library Reference

This document describes the modules under `firmware/src/` that together
implement the EPL Wire EDM Power Supply firmware. It is intended for a
developer who has just joined the project and wants to understand what
each module owns, how state is partitioned, and where to look when
adding a feature.

The firmware is **pure C11 against the Raspberry Pi Pico SDK**. There
is no Arduino runtime: stdio, ADC, PWM, GPIO, I2C, IRQ, and watchdog
all come from the SDK directly. All headers are wrapped in
`extern "C"` guards so individual translation units can be compiled as
C++ if ever needed.

For build / flash instructions see `firmware/README.md`. For the *why*
behind the current shape see `refactor-report.md`. For pin-level and
electrical detail see `config.h` and the schematics under
`circuit-boards/`.

---

## 1. Architecture at a glance

```
            ┌───────────────────────────────────────────────┐
            │  main.c — boot sequence, main loop,           │
            │           single GPIO IRQ dispatcher          │
            └────────┬─────────────┬─────────────┬──────────┘
                     │             │             │
                     ▼             ▼             ▼
                 ┌───────┐    ┌────────┐    ┌────────┐
                 │  cmd  │    │ fault  │    │  ctx   │
                 │ (USB- │    │ (latch │    │ (state │
                 │  CDC) │◄───┤ + spin │    │  hub)  │
                 └───┬───┘    │  loop) │    └────────┘
                     │        └────┬───┘
                     │             │
                     └─────┬───────┘
                           ▼
                       ┌────────┐
                       │ output │  output-stage PWM,
                       │        │  feedback PWM,
                       │        │  iso / edge mode bodies,
                       │        │  overcurrent ISR
                       └───┬────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
          ┌────────┐   ┌────────┐   ┌─────────┐
          │ boost  │   │  pmm   │   │ sensors │
          │ (DPOT/ │   │ (high- │   │  (ADC)  │
          │  I2C)  │   │  side) │   │         │
          └────────┘   └────────┘   └─────────┘

      shared types:        src/types.h
      hardware constants:  config.h
```

Three design rules drive the partitioning:

1. **`main.c` owns boot sequence, main loop, and the system-wide GPIO IRQ
   callback.** It is the only place that holds module-spanning runtime
   state (`g_ctx`, `g_boost_dpot`, `g_boost_cal`).
2. **Each `src/` module is a singleton with file-static state.** No
   transparent structs leak across module boundaries; ownership is
   one-peripheral-one-module. The `output` module is the only writer to
   the output-stage PWM slices; `boost` is the only thing that talks
   I2C to the DPOT; `sensors` is the only ADC consumer.
3. **Cross-module state lives in `main_ctx_t`** (defined in `ctx.h`) and
   is passed by pointer. No `extern` globals. ISR-shared fields are
   individually `volatile`.

Module APIs follow `module_func_desc()` naming. Constants are
`SCREAMING_SNAKE_CASE`. K&R braces. See `CLAUDE.md`.

---

## 2. Build layout

| File | Role |
|------|------|
| `main.c` | `int main(void)` — boot sequence, main loop, GPIO IRQ dispatcher. Owns `g_ctx`, `g_boost_dpot`, `g_boost_cal`. |
| `config.h` | Pin assignments (`*_PIN`), hardware constants, safe-operating limits, feature flags. **No runtime state.** |
| `CMakeLists.txt` | Pico SDK target definition, source list, link libs. |
| `src/types.h` | Shared enums (`device_state_t`, `mode_of_operation_t`, `fault_type_t`) and structs (`output_params_t`, `pwm_output_t`). |
| `src/ctx.{h,c}` | `main_ctx_t` and helpers (`ctx_init`, `ctx_set_state`, `ctx_set_mode`, `ctx_reset_discharge_stats`). |
| `src/sensors.{h,c}` | ADC drivers: PMM input current, output current, output voltage. Owns calibration offsets and the input-current running-average buffer. |
| `src/pmm.{h,c}` | Power-management module: high-side enable, inrush-wait, polled fault read. |
| `src/boost.{h,c}` | I2C driver for the boost-converter's TPL0401B digital potentiometer; owns the DPOT→voltage lookup table. |
| `src/output.{h,c}` | Phase-correct PWM for the four output-stage switches and the EDM_FEEDBACK signal; iso-frequency and edge-detection mode bodies; overcurrent ISR. |
| `src/fault.{h,c}` | ISR-thin / loop-thick fault model. Pending-fault flag, recovery dispatch, three ISR entry points. |
| `src/cmd.{h,c}` | USB-CDC stdio, command parser with static dispatch table, telemetry block. |

---

## 3. Shared types (`src/types.h`)

```c
typedef enum { STARTUP, FAULT, OPERATING, IDLE } device_state_t;

typedef enum { EDM_ISOFREQUENCY_MODE,
               EDGE_DETECTION_MODE } mode_of_operation_t;

typedef enum { PMM_FAULT_TYPE,
               BOOST_CONVERTER_PGOOD_FAULT,
               POWER_OUT_OF_RANGE_FAULT,
               HIGH_VOLTAGE_PHASE_SETUP_FAULT } fault_type_t;

typedef struct {
    int   discharge_count_target;   /* 0 = infinite */
    float duty_cycle;               /* 0.01 .. 0.12 */
    float frequency_hz;             /* 5000 .. 10000 */
    float init_voltage;             /* 64 .. 100 V */
} output_params_t;

typedef struct { uint32_t slice; uint32_t channel; } pwm_output_t;
```

`pwm_output_t` is shared so the output module can describe either of the
two channels (A/B) on a shared RP2040 PWM slice with the same descriptor.
`output_params_t` is the validated payload of `SET_ALL_PARAMETERS` and
the source of truth for live machining parameters (mirrored in
`main_ctx_t::params`).

---

## 4. Central runtime state — `main_ctx_t` (`src/ctx.h`)

```c
typedef struct {
    /* State machine */
    volatile device_state_t      state;
    volatile mode_of_operation_t mode;
    volatile bool                mode_prep_done;

    /* Machining parameters (mutated by SET_ALL_PARAMETERS) */
    volatile output_params_t     params;

    /* EDM iso-frequency discharge statistics (written by output ISR) */
    volatile bool     new_discharge_detected;
    volatile int      new_discharge_current_adc;
    volatile int      new_discharge_voltage_adc;
    volatile double   avg_discharge_current;
    volatile double   avg_discharge_voltage;
    volatile double   avg_discharge_success_rate;
    volatile double   discharge_success_rate;
    volatile int      current_discharge_count;
    volatile int      discharges_since_op_start;
    volatile bool     requested_discharges_reached;
    volatile uint32_t last_success_rate_calc_us;
    volatile int      discharge_rate_calc_interval_us;

    /* Edge-detection mode (written by output ISR) */
    volatile bool edge_detected;

    /* Periodic tick cursors (main loop only; not ISR-shared) */
    uint32_t last_periph_tick_ms;
    uint32_t last_telemetry_tick_ms;

    /* Feature flags (initialised from config.h constants) */
    bool allow_success_rate_pulse_skip;
    bool allow_power_setpoint_pulse_skip;
    bool periodic_telemetry_enabled;
} main_ctx_t;
```

`main.c` declares `static main_ctx_t g_ctx`. Every cross-module function
takes `main_ctx_t *ctx`. `volatile` is on the *fields* the ISRs touch,
not the pointer itself — passing a non-volatile pointer into ISR
context is intentional.

| Function | Effect |
|---|---|
| `ctx_init(ctx)` | Zero everything, set state=STARTUP, default machining params, copy feature flags from `config.h`. |
| `ctx_set_state(ctx, s)` | Direct state assignment. |
| `ctx_set_mode(ctx, m)` | Set mode and clear `mode_prep_done` so the next OPERATING entry re-preps. |
| `ctx_reset_discharge_stats(ctx)` | Clear all discharge-stat fields and `mode_prep_done`. Called by main loop after fault recovery. |

---

## 5. Module reference

### 5.1 `sensors` — ADC drivers

Owns three things:

- **Calibration offsets** for PMM input-current and output-current
  sensors. Captured at boot with no load on the sensor; subtracted from
  every subsequent ADC reading so 0 A reads as 0 A.
- **Input-current running-average buffer**
  (`DEVICE_CURRENT_BUFFER_SIZE = 100` samples). Filled by
  `sensors_sample_input_current()` once per peripheral-management tick
  (~10 ms) and read by the safety check and by telemetry.
- **Pure conversion helpers** for discharge ADC values captured inside
  the output-overcurrent ISR.

```c
void  sensors_init(void);
void  sensors_calibrate_input_current(void);   /* blocking, no-load */
void  sensors_calibrate_output_current(void);  /* blocking, no-load */

void  sensors_sample_input_current(void);
float sensors_avg_input_current_amps(void);
float sensors_read_output_voltage_averaged(int samples);

/* ISR fast paths */
int   sensors_read_output_current_adc(void);
int   sensors_read_output_voltage_adc(void);
float sensors_adc_to_discharge_current_amps(int adc);
float sensors_adc_to_discharge_voltage_volts(int adc);
```

ADC channel mapping is implicit in pin assignments: GPIO 26 = ADC0,
27 = ADC1, 28 = ADC2. Internally, every read does
`adc_select_input(pin - 26); adc_read()`.

Calibration order matters: PMM zero is taken with the high-side switch
*off*, then the switch is enabled and output-current zero is taken.
`main()` does this in the correct order.

### 5.2 `pmm` — power-management module switch

Three GPIOs control a separate board carrying a 48 V high-side switch
with built-in inrush limiting, overcurrent latch-off, and a diagnostic
fault output.

```c
void pmm_init(void);             /* directions, ENABLE deasserted, DIAG_EN asserted */
void pmm_enable_and_wait(void);  /* close the switch, sleep PMM_INRUSH_DELAY_MS */
void pmm_disable(void);
bool pmm_is_fault_active(void);  /* polled read; PMM_FAULT_PIN is active-low */
```

`PMM_FAULT_PIN`'s GPIO IRQ wiring is owned by the `fault` module + the
main.c dispatcher, not here.

### 5.3 `boost` — DPOT-driven HV rail

The boost converter's setpoint is controlled by a TPL0401B digital
potentiometer at I2C address `DPOT_ADDR` (0x3E). The module:

1. Initialises I2C0 (10 kHz, SDA=16, SCL=17).
2. Sweeps the DPOT 0..`BOOST_DPOT_POSITION_MAX` with the HV phase quietly
   enabled (no pulsing) and records the resulting rail voltage at each
   step into a `boost_cal_table_t`.
3. At runtime, translates a target voltage into the closest DPOT
   position via `boost_cal_lookup_wiper()` and ramps the wiper one
   position at a time so the rail rises smoothly.

```c
boost_dpot_t* boost_dpot_create(i2c_inst_t* port, uint8_t addr, uint8_t reg);
void          boost_setup_i2c  (boost_dpot_t* dpot, uint8_t sda, uint8_t scl, uint16_t baud);

void    boost_dpot_write_position(boost_dpot_t*, uint8_t pos);
uint8_t boost_dpot_read_position (boost_dpot_t*);
uint8_t boost_dpot_clamp_position(uint8_t pos);
float   boost_dpot_get_voltage   (const boost_dpot_t*, const boost_cal_table_t*);

boost_cal_status_t boost_cal_build(boost_dpot_t*, boost_cal_table_t*,
                                   int adc_samples, uint32_t settle_ms);
uint8_t            boost_cal_lookup_wiper(const boost_cal_table_t*, float target_v);
void               boost_set_voltage     (boost_dpot_t*, const boost_cal_table_t*,
                                          float target_v, uint32_t ramp_step_ms);
```

The lookup table is rebuilt on demand by `UPDATE_DPOT_VOLTAGE_TABLE`.
Failure leaves `cal->calibrated == false`; `main.c::verify_boost_cal_safe`
trips `HIGH_VOLTAGE_PHASE_SETUP_FAULT` if either the table never built
or its max measured voltage exceeded `MAX_HIGH_VOLTAGE_PHASE_VOLTS`.

### 5.4 `output` — output stage and mode bodies

The largest module: it absorbs the original `pwm_control` plus the
EDM iso-frequency / edge-detection state-machine bodies.

Drives five PWM channels across three slices:

| Pin | Slice/Ch | Polarity | Purpose |
|---|---|---|---|
| `SW_HIGH_VOLTAGE_PHASE_PIN` (8)  | 4A | inverted (P-ch) | HV pulse switch |
| `OUTPUT_OVERCURRENT_SET_PIN` (9) | 4B | normal          | Comparator threshold (PWM-as-DAC) |
| `SW_ENABLE_PIN` (10)             | 5A | normal (N-ch)   | Output stage enable |
| `SW_HIGH_CURRENT_PHASE_PIN` (11) | 5B | inverted (P-ch) | HC pulse switch |
| `EDM_FEEDBACK_PIN` (3)           | 1B | normal          | Power-ratio out to motion controller (frequency-encoded) |

```c
/* Lifecycle */
void output_init(void);
void output_attach_isr(main_ctx_t *ctx);
void output_overcurrent_isr(void);              /* called only by main.c dispatch */

/* Feedback PWM (motion-controller signal): power ratio is encoded as
 * PWM frequency at a fixed duty (EDM_FEEDBACK_PWM_DUTY). */
void output_feedback_set_ratio(float ratio);   /* 0..1 -> MIN..MAX freq */
void output_feedback_disable(void);            /* PWM off, line low */

/* Output stage */
void output_stage_setup(float duty, int freq_hz, float oc_threshold_a,
                        bool enable_main_slice, bool enable_hv_slice,
                        float hv_pulse_us, float hv_offset);
void output_stage_disable(void);

/* Mode entry (validates cal->calibrated; caller ramps boost first) */
bool output_setup_isofreq(main_ctx_t *ctx, const boost_cal_table_t *cal);
bool output_setup_edge   (const boost_cal_table_t *cal);

/* Per-tick mode bodies */
void output_run_isofreq(main_ctx_t *ctx);
void output_run_edge   (main_ctx_t *ctx);
```

Things that are easy to get wrong and therefore worth knowing:

1. **Phase-correct counter.** The output-stage slices use up-down
   counting, which doubles the period:
   `wrap = PWM_BASE_CLOCK_FREQ / (frequency_hz * 2)`.
2. **HV-phase counter offset.** The HV slice's counter is started at
   `wrap * EDM_ISOFREQ_HV_PWM_OFFSET` (currently 0.25) so the HV pulse
   and the HC pulse cannot overlap, preventing shoot-through between
   the two output switches.
3. **Slice grouping.** `SW_ENABLE_PIN` and `SW_HIGH_CURRENT_PHASE_PIN`
   share PWM slice 5; they cannot be enabled independently. Likewise
   `SW_HIGH_VOLTAGE_PHASE_PIN` and `OUTPUT_OVERCURRENT_SET_PIN` share
   slice 4. Hence the two enable booleans (`enable_main_slice`,
   `enable_hv_slice`) on `output_stage_setup`.
4. **P-channel inversion.** `pwm_set_output_polarity()` inverts the
   affected channels at the slice level. `output_stage_disable()` then
   bypasses PWM entirely and drives each gate to its safe-OFF level by
   hand: it reclaims each pad as `GPIO_FUNC_SIO` (without this the PWM
   peripheral still owns the pad and `gpio_put` is a no-op), then
   drives N-ch LOW and P-ch HIGH.
5. **Boost coupling.** `output_setup_isofreq` / `output_setup_edge` do
   *not* ramp the boost DPOT. The caller (main.c) must invoke
   `boost_set_voltage()` first; these functions only validate that the
   cal table is calibrated.
6. **Pulse-skip auto-rearm.** When iso-frequency mode triggers a
   pulse-skip pause, an internal helper re-arms the PWM slices with the
   current params after the pause. The original Arduino-era code
   disabled but never re-enabled, leaving the output dead until the
   next IDLE cycle.

`output_overcurrent_isr()` is the discharge-event handler:

- **Iso-freq:** snapshots `sensors_read_output_current_adc/voltage_adc`,
  bumps `ctx->current_discharge_count` and
  `ctx->discharges_since_op_start`, sets
  `ctx->new_discharge_detected`. Pure flag-set; no I/O.
- **Edge:** if `ctx->edge_detected` is unset, latches it and calls
  `output_stage_disable()` immediately. The main-loop body handles the
  signaling protocol.

### 5.5 `fault` — ISR-thin, loop-thick

Three sources, four types:

```c
void         fault_init(void);
void         fault_attach_irqs(void);

bool         fault_pending(void);
fault_type_t fault_active_type(void);
const char  *fault_type_name(fault_type_t);

void         fault_trip(fault_type_t type);              /* ISR-safe */
void         fault_handle(main_ctx_t *ctx,
                          bool (*cmd_poll_fn)(main_ctx_t *));

void         fault_pmm_isr(void);
void         fault_boost_pgood_isr(void);
```

The contract:

- **`fault_trip(type)` is universally safe.** It immediately calls
  `output_stage_disable()`, sets the active fault type, and raises the
  pending flag. ISRs call it; in-line detectors call it; the main-loop
  power-out-of-range check calls it.
- **`fault_handle()` runs only on the main loop.** It prints the fault
  name, defensively re-disables the output stage and clears the
  feedback duty, then dispatches by type:
  - `PMM_FAULT_TYPE` — 500 ms LED-off pause, recover.
  - `BOOST_CONVERTER_PGOOD_FAULT` — `fault_spin_until(boost_recovered)`.
  - `POWER_OUT_OF_RANGE_FAULT` / `HIGH_VOLTAGE_PHASE_SETUP_FAULT` —
    `fault_spin_until(NULL)`. Operator must `RESET_DEVICE`.
- **`cmd_poll_fn` is a function pointer** so the recovery spin can
  service operator commands without `fault.h` having to include
  `cmd.h`. `main.c` passes `cmd_poll`. NULL is allowed but disables
  operator interaction during recovery.

The two ISR entry points (`fault_pmm_isr`, `fault_boost_pgood_isr`)
are called only from the GPIO dispatcher in `main.c`. They are
one-liners that just call `fault_trip()` with the appropriate type.

The output-overcurrent ISR is in `output.c`, not here, because its
behaviour is mode-dependent.

### 5.6 `cmd` — USB-CDC command surface

Two halves:

- **Inbound (`cmd_poll`)** — non-blocking. Drains pending stdio bytes
  via `getchar_timeout_us(0)`, accumulates a 128-byte line, and on
  newline tokenizes via `strtok_r` and dispatches against a static
  table.
- **Outbound (`cmd_send_telemetry`)** — multi-line status block
  (firmware version, state, fault, input current/power, mode-specific
  stats and parameters). Called once per second from `cmd_tick` and
  on demand via the `SEND_TELEMETRY` command.

```c
void cmd_init(void);                                  /* stdio_init_all + 1s enum wait */
void cmd_set_boost_ctx(boost_dpot_t*, boost_cal_table_t*);  /* one-shot DI */
bool cmd_poll(main_ctx_t *ctx);                       /* non-blocking; dispatch one line */
void cmd_tick(main_ctx_t *ctx);                       /* periodic telemetry @ 1 Hz */
void cmd_send_telemetry(const main_ctx_t *ctx);
```

The dispatch table entry type lives in `cmd.c`:

```c
typedef struct {
    const char     *name;        /* exact-match keyword */
    int             argc_min;    /* required arg count, lower bound */
    int             argc_max;    /* required arg count, upper bound */
    cmd_handler_fn  handler;     /* called only after name + argc match */
    const char     *usage;       /* one-line syntax */
    const char     *desc;        /* one-line description for HELP */
} cmd_entry_t;
```

Dispatch rules:

- Names are matched **exactly** (no `startsWith` / prefix match).
- `argc` is checked against `[argc_min, argc_max]` before the handler
  is called; mismatches print the usage line and abort.
- Unknown commands print `ERROR: Unknown command 'X'` followed by the
  full HELP listing.
- All status/error messages use `OK: ...` / `ERROR: ...` prefixes.
- `HELP` (no args) prints the full listing; `HELP <CMD>` prints the
  usage and description for one entry.

| Command | Effect |
|---|---|
| `SEND_TELEMETRY` | Print the status block immediately. |
| `SET_ALL_PARAMETERS <discharges> <duty> <freq> <init_v>` | Validate against `config.h` SOA limits including computed on/off times, commit on success, clear `mode_prep_done`. |
| `EDGE_DETECTION_MODE` / `EDM_ISOFREQUENCY_MODE` | `output_stage_disable()`, `ctx_set_mode(...)`, print confirmation. |
| `RESET_DEVICE` | `output_stage_disable()`, drain stdio, `watchdog_reboot(0,0,0)`. |
| `SET_DPOT <pos>` | Direct DPOT write (debug). |
| `READ_HVP_VOLTAGE` | 10-sample averaged HV-rail read. |
| `UPDATE_DPOT_VOLTAGE_TABLE` | Re-run the boost cal sweep. |
| `SET_DPOT_FROM_VTABLE <volts>` | Ramp DPOT to closest table entry. |
| `SET_FEEDBACK_RATIO <0..1>` | Override the EDM_FEEDBACK power ratio (encoded as PWM frequency) (debug). |
| `HELP [<command>]` | List all commands, or print full usage for one. |

Validation in `apply_parameters()` is the only path through which the
device's output behaviour changes via serial. It validates against
`MIN_/MAX_DUTY_CYCLE`, `MIN_/MAX_MACHINING_FREQUENCY_HZ`,
`MIN_/MAX_INIT_VOLTAGE`, computed on-time within
`MIN_/MAX_ON_TIME_MICROS`, and computed off-time at least
`MIN_OFF_TIME_MICROS`. On success it sets `ctx->mode_prep_done = false`
so the next OPERATING entry re-applies the new values.

---

## 6. Lifecycle

### 6.1 Boot (`int main(void)`)

1. `ctx_init(&g_ctx)`.
2. `cmd_init()` — `stdio_init_all` + 1 s USB-CDC enumeration delay; print
   `Software Version: 1.0-beta`.
3. STATUS LED + EDM_ENABLE GPIO directions.
4. `output_init()` — cache PWM slice/channel descriptors, configure
   feedback PWM, force the four output switches into safe-OFF SIO state.
5. `pmm_init()` — directions, ENABLE deasserted, DIAG_EN asserted.
6. `sensors_init()` — `adc_init` + `adc_gpio_init` for the three sensor
   pins.
7. `boost_dpot_create` + `boost_setup_i2c`; `cmd_set_boost_ctx()` injects
   the dpot and cal table into the cmd module.
8. `sensors_calibrate_input_current()` — PMM zero with switch off.
9. `pmm_enable_and_wait()` — close the high-side switch, sleep
   `PMM_INRUSH_DELAY_MS`.
10. `sensors_calibrate_output_current()` — output-side zero now that the
    bus is alive.
11. `gpio_set_irq_callback(gpio_irq_dispatch); irq_set_enabled(IO_IRQ_BANK0, true)`
    — register the system-wide GPIO callback **once**. Per-pin enables
    in `fault_attach_irqs` and `output_attach_isr` use the no-callback
    variant and attach to this dispatcher.
12. `fault_init() + fault_attach_irqs()` — arms PMM_FAULT and BOOST_PGOOD
    edge IRQs.
13. `output_attach_isr(&g_ctx)` — binds ctx, arms OUTPUT_OVERCURRENT.
14. **HV-only output stage** brought up so the boost can be characterised.
15. `boost_cal_build(g_boost_dpot, &g_boost_cal, 8, 20)`.
16. `verify_boost_cal_safe()` — trips `HIGH_VOLTAGE_PHASE_SETUP_FAULT` if
    the cal table never built or its max measured voltage exceeds
    `MAX_HIGH_VOLTAGE_PHASE_VOLTS`.
17. `output_stage_disable()`.
18. If a fault is pending, print "ERROR: setup failed; fault pending"
    and let iteration 1 of the main loop dispatch it. Otherwise
    STATUS_LED on, "OK: Setup complete", state → IDLE.

### 6.2 Main loop

```c
while (1) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (fault_pending()) {                    // (1) faults take priority
        ctx_set_state(&g_ctx, FAULT);
        fault_handle(&g_ctx, cmd_poll);
        ctx_reset_discharge_stats(&g_ctx);
        ctx_set_state(&g_ctx, IDLE);
        continue;
    }

    cmd_poll(&g_ctx);                         // (2) commands every iteration

    if (now_ms - g_ctx.last_periph_tick_ms    // (3) housekeeping @ 100 Hz
        >= PERIPH_MGMT_INTERVAL_MS) {
        periph_tick();                        //     PMM sample,
        g_ctx.last_periph_tick_ms = now_ms;   //     power envelope,
    }                                         //     EDM_ENABLE → state,
                                              //     feedback duty
    switch (g_ctx.state) {                    // (4) state body
    case OPERATING:
        if (!g_ctx.mode_prep_done)
            prepare_isofreq_mode() / prepare_edge_mode();
        if (g_ctx.mode_prep_done)
            output_run_isofreq() / output_run_edge();
        break;
    case IDLE:
        output_stage_disable();
        g_ctx.mode_prep_done = false;
        g_ctx.discharges_since_op_start = 0;
        sleep_ms(10);
        break;
    case STARTUP:
    case FAULT:
        break;
    }

    cmd_tick(&g_ctx);                         // (5) telemetry @ 1 Hz
}
```

Order matters:

- **Faults first.** Recovery may run for a long time; nothing else
  should attempt to re-arm hardware while a fault is pending.
- **`cmd_poll` every iteration.** No periodic gating — operators get
  prompt response. Latency is whatever the slowest iteration takes
  (`sleep_ms(10)` in IDLE, much shorter in OPERATING).
- **Periph tick at 100 Hz.** Polls EDM_ENABLE_PIN to drive the
  IDLE↔OPERATING transition, watchdogs the input-power envelope, and
  keeps the feedback signal in sync with measured input current.
- **Mode prep at OPERATING entry.** `prepare_isofreq_mode` /
  `prepare_edge_mode` ramp the boost (`boost_set_voltage`) and call the
  matching `output_setup_*`. `mode_prep_done` is only latched true if
  prep didn't trip a fault.

### 6.3 GPIO IRQ wiring

The RP2040 SDK only allows **one system-wide GPIO IRQ callback**. Every
edge-triggered pin in this firmware (`PMM_FAULT_PIN`, `BOOST_PGOOD_PIN`,
`OUTPUT_OVERCURRENT_PIN`) routes through `gpio_irq_dispatch` in
`main.c`, which dispatches by pin to one of:

```
fault_pmm_isr()         ← PMM_FAULT_PIN falling edge
fault_boost_pgood_isr() ← BOOST_PGOOD_PIN falling edge
output_overcurrent_isr()← OUTPUT_OVERCURRENT_PIN falling edge
```

Modules expose these `_isr` entry points but do not register them with
the SDK. The default GPIO callback dispatcher acknowledges edge IRQs
automatically; ISRs do not need to call `gpio_acknowledge_irq`.

### 6.4 Operating modes

**EDM iso-frequency** (`output_run_isofreq`):

- Drains `ctx->new_discharge_detected` flag set by the ISR; folds I/V
  ADCs through `sensors_adc_to_discharge_*` and into 0.9/0.1 EWMAs.
- At each calc-window boundary (every `DISCHARGES_PER_CALC_INTERVAL`
  discharges or `discharge_rate_calc_interval_us`, whichever first),
  recomputes `discharge_success_rate` and the 0.99/0.01 EWMA
  `avg_discharge_success_rate`.
- Two pulse-skip guards (gated by `ctx->allow_*_pulse_skip`):
  - Success rate ≥ `MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD` (0.8) →
    disable, 250 ms wait, re-arm.
  - Avg input power > `MAX_INPUT_POWER_SETPOINT_WATTS` (75 W) →
    disable, 10 ms wait, re-arm.
- Honours `params.discharge_count_target` (0 = infinite). Latches
  `requested_discharges_reached` when reached.

**Edge detection** (`output_run_edge`):

- Drains `ctx->edge_detected` (latched by the ISR with the output
  already disabled).
- Drives feedback to `EDM_FEEDBACK_FREQ_MAX_HZ` (tone burst) for 1 s,
  then disables the feedback PWM (silence) for 1 s, then clears the
  flag. After detection the output stays disabled until the device
  cycles through IDLE.

---

## 7. Conventions for new code

When adding to this codebase:

- **Hardware constants and tunables go in `config.h`.** No runtime
  state in `config.h`.
- **Shared enums or structs go in `src/types.h`.** Module-private types
  stay module-private (file-static or in the `.c`).
- **Cross-module state goes in `main_ctx_t`** (`ctx.h`). ISR-shared
  fields must be individually `volatile`.
- **One peripheral, one module.** If you find yourself touching ADC
  pins outside `sensors.c`, PWM slices outside `output.c`, or I2C
  outside `boost.c`, move it.
- **Every header is `extern "C"` guarded** — `#ifdef __cplusplus`
  / `extern "C" {`.
- **Functions that may be called from an ISR must be ISR-safe** — no
  `printf`, no `sleep_ms`, no I2C. Pattern: ISR sets a flag (or pushes
  ADC samples into volatile state), main loop consumes.
- **New fault sources:** extend `fault_type_t` in `types.h`, add a
  `_isr` entry point in `fault.c` (or wherever the pin lives), add a
  case to `fault_handle()` so the recovery semantics are explicit, and
  a branch in `gpio_irq_dispatch()` if the source is a new GPIO.
- **New commands:** add a `cmd_handle_*` static handler in `cmd.c`, an
  entry in the `g_cmd_table[]` with `[argc_min, argc_max]`, `usage`,
  and `desc`. The strict-argc dispatcher will validate before invoking
  the handler.
- **Pin constants:** all pin numbers in `config.h` carry a `_PIN`
  suffix.
- **Naming:** `module_func_desc()`. Constants `SCREAMING_SNAKE_CASE`.
  K&R braces. Use `float` for sensor conversions (not `double`) unless
  there is a specific accuracy reason to widen.
