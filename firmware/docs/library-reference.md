# EPL WPS Firmware — Library Reference

This document describes the library of modules under `firmware/src/` that
together implement the EPL Wire EDM Power Supply firmware. It is intended
for a developer who has just joined the project and wants to understand
what each module owns, how state is partitioned, and where to look when
adding a feature.

For the *why* behind the current shape (what was refactored, what was
broken in the original) see `refactor-report.md`. For pin-level and
electrical detail, see `config.h` and the schematics under
`circuit-boards/`.

---

## 1. Architecture at a glance

```
                  firmware.ino  (top level — owns device state)
                  /     |      \
                 /      |       \
        ┌───────┴──┐ ┌──┴────┐ ┌─┴────────────┐
        │ telemetry│ │ fault │ │ peripheral   │
        │ (serial) │ │handler│ │ management   │
        └────┬─────┘ └───┬───┘ └──────┬───────┘
             │           │            │
             ▼           ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ pwm_     │ │ boost_   │ │ sensors  │
        │ control  │ │ module   │ │ (ADC)    │
        └──────────┘ └──────────┘ └──────────┘
                       │
                       ▼
                 [TPL0401B DPOT
                   over I²C]

      shared types: src/types.h
      hardware constants & limits: config.h
```

Two design rules drive the partitioning:

1. **`firmware.ino` owns device-level state.** Anything modal, statistical,
   or operator-controlled (mode, parameters, discharge stats, edge flag)
   lives there. Sub-modules read it via `extern` rather than mirroring it.
2. **Sub-modules own their hardware.** Each `src/` module is the *only*
   thing that touches its peripheral. `pwm_control` is the only writer to
   the output-stage PWM slices; `boost_module` is the only thing that
   talks I²C to the DPOT; `sensors` is the only ADC consumer.

Module APIs follow `module_func_desc()` naming. Constants are
`SCREAMING_SNAKE_CASE`. K&R braces. See `CLAUDE.md`.

---

## 2. Build layout

| File | Role |
|------|------|
| `firmware.ino` | Arduino entry points (`setup`, `loop`); device-state owner; mode handlers; output-overcurrent ISR dispatcher |
| `config.h` | Pin assignments, hardware constants, safe-operating limits, feature flags. **No runtime state.** |
| `src/types.h` | Shared enums (`DeviceState`, `ModeOfOperation`, `FaultStateType`) and structs (`OutputParameters`, `PWMOutput`) |
| `src/sensors.{h,cpp}` | ADC drivers: PMM input current, output current, output voltage. Owns calibration offsets and the input-current running-average buffer |
| `src/pwm_control.{h,cpp}` | Phase-correct PWM for the four output-stage slices and the EDM_FEEDBACK signal to the motion controller |
| `src/boost_module.{h,cpp}` | I²C driver for the boost-converter's TPL0401B digital potentiometer; owns the DPOT→voltage lookup table |
| `src/fault_handler.{h,cpp}` | ISR-thin / loop-thick fault model. Attaches PMM and BOOST_PGOOD ISRs, owns the pending-fault flag, dispatches recovery |
| `src/telemetry.{h,cpp}` | Serial command parser and periodic status block. Reads device state via `extern` from `firmware.ino` |

---

## 3. Shared types (`src/types.h`)

```c
enum DeviceState     { STARTUP, FAULT, OPERATING, IDLE };
enum ModeOfOperation { EDM_ISOFREQUENCY_MODE, EDGE_DETECTION_MODE };
enum FaultStateType  { PMM_FAULT_TYPE,
                       BOOST_CONVERTER_PGOOD_FAULT,
                       POWER_OUT_OF_RANGE_FAULT,
                       HIGH_VOLTAGE_PHASE_SETUP_FAULT };

struct OutputParameters {
    int   dischargeCountTarget;   // 0 = infinite
    float dutyCycle;              // 0.01 .. 0.12
    float frequency;              // 5000 .. 10000 Hz
    float initVoltage;            // 64 .. 100 V
};

struct PWMOutput { uint32_t slice; uint32_t channel; };
```

`PWMOutput` exists so the same descriptor can refer to either of the two
channels (A/B) on a shared RP2040 PWM slice. `OutputParameters` mirrors
the payload of the `SET_ALL_PARAMETERS` serial command.

---

## 4. Device-level state (lives in `firmware.ino`)

`telemetry.cpp` imports these via `extern` and they are documented at the
top of that file. They are deliberately not encapsulated in a class —
they are read and written across module boundaries (mode handlers,
ISRs, telemetry).

| Variable | Owner-writes | Notes |
|---|---|---|
| `deviceState` | `loop()`, `handle_peripheral_management()` | `STARTUP`→`IDLE` after setup, then driven by `EDM_ENABLE` pin |
| `modeOfOperation` | `telemetry_process_command()` | Switched between iso-freq and edge-detection by the host |
| `modePreparationComplete` | mode handlers, telemetry on mode change | Forces mode-entry re-init (boost ramp, PWM reconfigure) |
| `dischargeCountTarget`, `machiningDutyCycle`, `machiningFrequency`, `machiningInitVoltage` | `apply_parameters()` in telemetry | Validated against limits in `config.h` before commit |
| `avgDischarge*`, `dischargesSinceOperationStarted` | `edm_isofreq_mode()`, overcurrent ISR | EWMAs over discharge events |
| `edgeDetected` | edge-detection ISR | One-shot flag for edge mode |
| `newDischargeDetected`, `newDischargeCurrent_ADC`, `newDischargeVoltage_ADC` | overcurrent ISR | Raw ADC handoff from ISR to main-loop EWMA update |

All are `volatile`. Anything touched from an ISR must remain `volatile`.

---

## 5. Module reference

### 5.1 `sensors` — ADC drivers

Owns three things:

- **Calibration offsets** for PMM input-current and output-current sensors.
  Captured at boot with no load on the sensor; subtracted from every
  subsequent ADC reading so `0 A` reads as `0 A`.
- **Input-current running-average buffer** (`DEVICE_CURRENT_BUFFER_SIZE = 100`
  samples). Filled by `sensors_sample_input_current()` once per peripheral-
  management tick (~10 ms) and read by the safety check and by telemetry.
- **Pure conversion helpers** for discharge ADC values captured inside the
  output-overcurrent ISR.

```c
void   sensors_setup_adc(void);
void   sensors_calibrate_input_current(void);   // blocking, no-load
void   sensors_calibrate_output_current(void);  // blocking, no-load
void   sensors_sample_input_current(void);      // call from main loop tick
float  sensors_avg_input_current(void);         // A; 0.0 until buffer first fills
double sensors_adc_to_discharge_current(int adc); // A
double sensors_adc_to_discharge_voltage(int adc); // V
int    sensors_read_output_voltage_averaged(int samples);  // V
```

Calibration order matters: PMM zero is taken with the high-side switch
*off*, then the switch is enabled and output-current zero is taken.
`firmware.ino:setup()` does this in the correct order.

The averaged HV read is what `boost_module` uses while building the
DPOT lookup table.

### 5.2 `pwm_control` — output-stage and feedback PWM

Drives five PWM channels across three slices:

| Pin | Slice/Ch | Polarity | Purpose |
|-----|----------|----------|---------|
| `SW_HIGH_VOLTAGE_PHASE` (8) | 4A | inverted (P-ch) | HV pulse switch |
| `OUTPUT_OVERCURRENT_SET` (9) | 4B | normal | Comparator threshold (PWM-as-DAC) |
| `SW_ENABLE` (10) | 5A | normal (N-ch) | Output stage enable |
| `SW_HIGH_CURRENT_PHASE` (11) | 5B | inverted (P-ch) | HC pulse switch |
| `EDM_FEEDBACK` (3) | 1B (varies) | normal | Power ratio out to motion controller |

Three things that are easy to get wrong and therefore worth knowing:

1. **Phase-correct counter.** The output-stage slices use up-down counting,
   which doubles the period. So `wrap = f_clk / (f_out * 2)`, not
   `f_clk / f_out`.
2. **HV-phase counter offset.** The HV slice's counter is started at
   `wrap * EDM_ISOFREQ_HV_PWM_OFFSET` (currently 0.25) so that the HV
   pulse and the HC pulse cannot overlap, preventing shoot-through
   between the two output switches.
3. **P-channel inversion.** P-channel MOSFETs are OFF when their gate is
   HIGH. `pwm_set_output_polarity()` inverts the affected channels at
   the slice level, but `pwm_disable_output_stage()` then bypasses PWM
   entirely and drives each gate to its safe-OFF level by hand —
   N-ch LOW, P-ch HIGH.

```c
void pwm_setup_feedback(void);                  // 1 kHz, /10 divider
void pwm_set_feedback_duty(double duty);        // clamped [0, 1]

void pwm_setup_output_stage(double machining_duty_cycle,
                            int    machining_frequency_hz,
                            int    output_current_threshold_a,
                            bool   enable_switch,
                            bool   high_current_phase,
                            bool   high_voltage_phase,
                            int    hv_pulse_on_time_us,
                            double hv_pwm_offset);

void pwm_disable_output_stage(void);            // safe-OFF every switch
```

`pwm_disable_output_stage()` is the universal "back off" call. It is
invoked by `fault_trip()`, by mode-change commands, by the idle handler,
and by the iso-freq pulse-skipping path.

### 5.3 `boost_module` — DPOT-driven HV rail

The boost converter's setpoint is controlled by a TPL0401B digital
potentiometer at I²C address 0x3E. The module:

1. Initialises I²C (`I2C0`, 10 kHz, SDA=16, SCL=17).
2. At startup, sweeps the DPOT 0…110 with the HV phase quietly enabled
   (no pulsing) and records the resulting rail voltage at each step.
   The lookup table lives in module-private static storage.
3. At runtime, translates a target voltage into the DPOT position whose
   *measured* voltage is closest, with a 5 V tolerance — anything worse
   trips `HIGH_VOLTAGE_PHASE_SETUP_FAULT`.

```c
void    boost_setup_i2c(void);
uint8_t boost_dpot_read(void);
void    boost_dpot_write(int position);    // clamped to [0, 110]
void    boost_build_voltage_table(void);   // ~3 s blocking, called from setup
void    boost_set_voltage(int target_volts, int spacing_ms);
```

The table is rebuilt on demand by the `UPDATE_DPOT_VOLTAGE_TABLE` serial
command. `boost_set_voltage` ramps the DPOT one position at a time with
`spacing_ms` between writes — that's how the rail rises smoothly rather
than slamming up.

If the boost module is unpopulated, broken, or its rail can't reach the
maximum HV setpoint, the table-build call is what catches it: the echo-
read of a probe write fails, or the maximum measured voltage falls
short of `MAX_HIGH_VOLTAGE_PHASE_VOLTS`. Either way, `fault_trip()`
fires before any pulsing is attempted.

### 5.4 `fault_handler` — ISR-thin, loop-thick

This module is the answer to the original firmware's habit of doing
serial I/O and busy-waits inside ISRs. It replaces them with a single
flag.

```c
void fault_attach_interrupts(void);          // PMM_FAULT, BOOST_PGOOD
void fault_trip(FaultStateType type);        // safe to call from ISR or loop
bool fault_pending(void);
FaultStateType fault_current_type(void);
void fault_handle(void);                     // call from loop()
const char *fault_type_name(FaultStateType type);
```

The contract:

- **`fault_trip(type)` is universally safe.** It immediately disables the
  output stage, sets the active fault type, and raises `pending`. ISRs
  call it; in-line detectors (like `boost_build_voltage_table`) call it;
  the main-loop power-out-of-range check calls it.
- **`fault_handle()` runs only on the main loop.** It prints the fault
  name, then dispatches by type:
  - `PMM_FAULT_TYPE` — 500 ms LED blink, then clear (recoverable).
  - `BOOST_CONVERTER_PGOOD_FAULT` — service serial commands until the
    PGOOD line goes high again.
  - `POWER_OUT_OF_RANGE_FAULT`, `HIGH_VOLTAGE_PHASE_SETUP_FAULT` —
    non-recoverable: only `RESET_DEVICE` will exit. The handler runs
    a serial-command loop with no recovery predicate.

The ISRs (`isr_pmm`, `isr_boost`) acknowledge their GPIO IRQ and then
call `fault_trip()`. Nothing else happens in ISR context.

The output-overcurrent ISR is wired by `firmware.ino` rather than this
module because its behaviour is mode-dependent (capture-and-update for
iso-freq, set-edge-flag for edge detection).

### 5.5 `telemetry` — serial command surface

Two halves:

- **Outbound:** `telemetry_send()` prints a multi-line status block —
  firmware version, device state, fault type (if any), input current
  and computed input power, then either the edge-detection or
  iso-frequency stats and parameters depending on mode. Called once
  per second from `loop()`, and on demand via the `SEND_TELEMETRY`
  command.
- **Inbound:** `telemetry_process_command(String)` parses one trimmed
  command line.

| Command | Effect |
|---|---|
| `SEND_TELEMETRY` | Emit status block immediately |
| `SET_ALL_PARAMETERS <n> <duty> <freq> <initV>` | Validate against `config.h` limits, compute on/off times, commit on success |
| `EDGE_DETECTION_MODE` | Disable output stage, switch mode, force re-init |
| `EDM_ISOFREQUENCY_MODE` | Disable output stage, switch mode, force re-init |
| `RESET_DEVICE` | Disable output stage, then `rp2040.reboot()` |
| `SET_DPOT <n>` | Direct DPOT write (debug) |
| `READ_HVP_VOLTAGE` | Averaged HV rail read |
| `UPDATE_DPOT_VOLTAGE_TABLE` | Rebuild the boost lookup table |
| `SET_DPOT_FROM_VTABLE <V>` | Ramp boost to a target voltage via the table |
| `SET_FEEDBACK_DUTY <0..1>` | Override the EDM_FEEDBACK signal (debug) |
| anything else | Print help |

`apply_parameters()` is the single point that mutates the four
machining parameters; it is the only path through which the device's
output behaviour changes via serial. It validates against:

- `MIN_DUTY_CYCLE` / `MAX_DUTY_CYCLE`
- `MIN_MACHINING_FREQUENCY_HZ` / `MAX_MACHINING_FREQUENCY_HZ`
- `MIN_INIT_VOLTAGE` / `MAX_INIT_VOLTAGE`
- Computed on-time within `MIN_ON_TIME_MICROS`..`MAX_ON_TIME_MICROS`
- Computed off-time at least `MIN_OFF_TIME_MICROS`

On success it sets `modePreparationComplete = false` so the next loop
iteration in `OPERATING` re-applies the new values.

---

## 6. Lifecycle

### 6.1 `setup()`

1. Set pin modes (digital + analog inputs, output drivers).
2. Drive PMM_ENABLE low, PMM_DIAG_EN high.
3. `sensors_setup_adc()` — 12-bit ADC.
4. `pwm_setup_feedback()`, `pwm_disable_output_stage()` — feedback PWM
   live, output switches in safe-OFF.
5. `sensors_calibrate_input_current()` — PMM zero with switch off.
6. Enable PMM, wait `PMM_INRUSH_DELAY_MS` for inrush settling.
7. `sensors_calibrate_output_current()` — output-side zero now that the
   bus is alive.
8. `telemetry_setup_serial()` — Serial @ 115200, brief enumerate wait.
9. `fault_attach_interrupts()` and the output-overcurrent ISR.
10. `boost_setup_i2c()`, `boost_build_voltage_table()` — characterises
    the HV rail; this is also the first opportunity to fault on a
    missing or faulty boost module.
11. `STATUS_LED` on; `deviceState = IDLE`.

### 6.2 `loop()`

```c
void loop(void) {
    unsigned long now_ms = millis();

    if (fault_pending()) {                     // (1) handle faults first
        deviceState = FAULT;
        fault_handle();
        deviceState = IDLE;
    }
    if (now_ms - last_periph >= 10) {           // (2) housekeeping @ 100 Hz
        handle_peripheral_management();          //     sample current,
        last_periph = now_ms;                    //     update statistics,
    }                                            //     read EDM_ENABLE,
                                                 //     update feedback duty,
                                                 //     drain serial input
    switch (deviceState) {                       // (3) mode body
        case OPERATING: handle_operating_state(); break;
        case IDLE:      handle_idle_state();      break;
        default:        break;
    }
    if (sendPeriodicTelemetryEnabled            // (4) telemetry @ 1 Hz
        && now_ms - last_tlm >= 1000) {
        telemetry_send();
        last_tlm = now_ms;
    }
}
```

The order matters: fault handling runs first so that any latched fault
is serviced before the operating-state body could re-arm hardware. The
peripheral tick is what watchdogs the input-power envelope and what
keeps the feedback signal to the motion controller in sync with the
measured input current.

### 6.3 Operating modes

**EDM iso-frequency** (`edm_isofreq_mode` in `firmware.ino`):
- On entry, ramp boost to `machiningInitVoltage`, then call
  `pwm_setup_output_stage()` with all three switches enabled and the
  HV phase offset set to `EDM_ISOFREQ_HV_PWM_OFFSET`.
- The output-overcurrent ISR captures the I/V ADCs at each discharge
  and increments two counters; the main loop EWMAs the I/V and computes
  a per-window discharge success rate.
- Two pulse-skip guards: success-rate too high (gap conditions are
  too good — back off briefly) and average input power above the
  setpoint (back off very briefly).
- If `dischargeCountTarget` is non-zero and reached, output stage is
  disabled and the run is considered complete.

**Edge detection** (`edge_detection_mode`):
- Boost to `MIN_HIGH_VOLTAGE_PHASE_VOLTS`, configure the output stage
  with HV phase off and a fixed 5%/10 kHz duty.
- The first overcurrent event sets `edgeDetected`, disables the
  output, drives feedback to 1.0 for a second to signal the motion
  controller, then resets after another second.

---

## 7. Conventions for new code

When adding to this codebase:

- Hardware constants and tunables go in `config.h`. **No runtime state
  in `config.h`.**
- Shared enums or structs go in `src/types.h`. Module-private types stay
  module-private.
- One peripheral, one module. If you find yourself touching ADC pins
  outside `sensors.cpp` or PWM slices outside `pwm_control.cpp`, move it.
- Functions that may be called from an ISR must be ISR-safe — no
  `Serial.print`, no `delay`, no I²C. The pattern is: ISR sets a flag
  (or pushes ADC samples into volatile state), main loop consumes.
- New fault sources: extend `FaultStateType`, attach the ISR in
  `fault_attach_interrupts()`, and add a case to `fault_handle()` so
  the recovery semantics are explicit.
- New serial commands: add a branch in `telemetry_process_command()`
  and a help line in `print_help()`.
- Function names lead with the module: `sensors_*`, `pwm_*`, `boost_*`,
  `fault_*`, `telemetry_*`. Constants `SCREAMING_SNAKE_CASE`. K&R braces.
