# EPL Wire EDM Power Supply — Firmware Refactor Report

**Branch:** `firmware-refactor`
**Scope:** `firmware/`
**Target:** RP2040 (Raspberry Pi Pico) via `arduino-pico` (earle-philhower) board package
**Baseline:** `firmware.ino` (1530 lines, single file, would not compile)
**Result:** `firmware.ino` (336 lines) + 5 modules under `src/` (~900 lines total), clean compile at 74820 B flash / 10244 B RAM (3% / 3%)

---

## 1. Summary of Changes

The refactor was sequenced in six stages, each landing as its own commit so the build stayed green between steps and any individual stage could be reverted independently.

### Stage 1 — Baseline bug-fix pass
Make the existing firmware compile and behave correctly before changing any structure. See section 2 for details.

### Stage 2 — Extract `config.h` and `src/types.h`
Pull every pin assignment, hardware constant, scaling factor, and safe-operating limit out of `firmware.ino` into `firmware/config.h`. Pull every shared `enum` and `struct` (`DeviceState`, `ModeOfOperation`, `FaultStateType`, `OutputParameters`, `PWMOutput`) into `firmware/src/types.h`. After this stage, no runtime state lives in either file — they are header-only declarations.

### Stage 3 — Module split into `src/`
Carve `firmware.ino` into five paired `.h`/`.cpp` modules with `module_func_desc` naming:

| Module | Responsibility |
|---|---|
| `sensors` | ADC setup, calibration offsets, input-current running average, ADC→engineering-unit conversions |
| `pwm_control` | EDM-feedback PWM and output-stage PWM setup (4 shared-slice channels), safe-OFF disable |
| `boost_module` | TPL0401B DPOT driver over I2C, voltage-lookup-table build at startup, `boost_set_voltage` ramping |
| `fault_handler` | Pending/active fault state, ISR registration, main-loop `fault_handle()` dispatcher |
| `telemetry` | `Serial.begin` lifecycle, command parser, `SEND_TELEMETRY` block formatter, parameter validation |

`firmware.ino` shrank from ~1530 lines to 368, retaining only the device-level globals, `setup()`/`loop()`, and the two operating-mode bodies (iso-frequency, edge detection) plus their overcurrent ISRs.

### Stage 4 — Cull dead code and redundant state
Removed a layer of obsolete artifacts left over from the buck-converter era of the hardware:

- `BUCK_POWER_GOOD`, `BUCK_ENABLE`, `BUCK_ADJ_PWM` pin definitions (the buck stage was replaced by a pi-filter board)
- `BUCK_CONVERTER_PGOOD_FAULT` enum and its handler
- `PMM_FAULT_IN_USE`, `HIGH_CURRENT_PGOOD_IN_USE`, `HIGH_VOLTAGE_PGOOD_IN_USE` feature flags (always set to constants in practice)
- Unused `pmm_recovered()` helper
- Empty `types.cpp` stub left over from Stage 2
- Inlined `enablePortStatus` write/read pair into a single `digitalRead(EDM_ENABLE)` at the call site
- Removed duplicate `gpio_set_function(..., GPIO_FUNC_PWM)` calls (PWM init already configures the pin)
- Promoted two magic numbers (`DISCHARGES_PER_CALC_INTERVAL`, `MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD`) to named `config.h` constants

### Stage 5 — Simplify state machine
The original device-state machine carried two redundant abstractions: a `PERIPHERAL_MANAGEMENT` device-state used as a "do periodic work then return" trampoline, and an `oldDeviceState` shadow used to restore state after fault recovery. Both were removed:

- Dropped the `PERIPHERAL_MANAGEMENT` enumerator. Periodic peripheral work is now an unconditional periodic call from `loop()` gated by `peripheralManagementInterval_MS`, not a state.
- Dropped `oldDeviceState`. After fault recovery the loop sets `deviceState = IDLE`; the next peripheral tick (10 ms later) re-derives `OPERATING`/`IDLE` from `EDM_ENABLE` directly.
- Dropped the global `currentFaultType` variable. `fault_handler` owns this internally; `telemetry` queries it via `fault_current_type()` on demand.
- Inlined the former `read_enable_port()` shim — it previously populated a global flag that was read elsewhere; now it's a local `digitalRead`.

### Stage 6 — Final verify
Clean compile pass with `--warnings all`. Greppable confirmation that all removed identifiers (`BUCK_*`, `PERIPHERAL_MANAGEMENT`, `oldDeviceState`, `currentFaultType`, `*_IN_USE`, `pmm_recovered`, `enablePortStatus`, `read_enable_port`) appear only in archived reference files (`stale_firmware.txt`, `notes.md`, `functional-decomp.md`), not in live code. Naming conventions audited against CLAUDE.md (`module_func_desc`, `SCREAMING_SNAKE_CASE` constants, K&R braces).

---

## 2. Issues Found in Original Firmware

### 2.1 Build-breaking errors

**Missing closing brace in `READ_HVP_VOLTAGE()`.**
The function body ended with `Serial.println(avg, 3);` and no `}`. Every subsequent free function in the file was therefore parsed as a nested function definition, producing a cascade of errors and a non-buildable tree. Fix: close the brace.

**Variable shadowing the enum it stores.**
The global was declared `volatile FaultStateType FaultStateType = PMM_FAULT_TYPE;` — the variable name was identical to the type name. Some toolchains accept this with warnings; others reject it outright. Fix: renamed to `currentFaultType` (and later removed entirely in Stage 5 in favor of `fault_handler` ownership).

### 2.2 Latent runtime bugs

**Acknowledging the wrong GPIO in HV fault path.**
`handleHighVoltagePhaseFault()` and `handleHighVoltagePhaseSetupFault()` called `gpio_acknowledge_irq(BOOST_CONVERTER_PGOOD_FAULT, ...)` and polled `gpio_get(BOOST_CONVERTER_PGOOD_FAULT)`. `BOOST_CONVERTER_PGOOD_FAULT` is the `FaultStateType` enum value (an int constant for a fault classification), **not** a GPIO number. The acknowledge therefore hit an unrelated pin and the recovery wait spin-looped against the wrong input. The active boost-PGOOD interrupt was never acknowledged, so any subsequent fall-edge would be missed. Fix: pass `BOOST_PGOOD` (GPIO 18). In Stage 3 this path moved into `fault_handler.cpp` where the ISR/recovery pair is paired explicitly.

**Fault type never printed by telemetry.**
`sendTelemetry()` gated the fault-string lookup on a flag named `isInFaultState` that was declared but **never assigned anywhere in the codebase**. The telemetry block therefore always reported `FAULT NONE` even when `deviceState == FAULT`. Fix: replaced the unused flag with `deviceState == FAULT` directly. After Stage 5, the test reads `if (deviceState == FAULT) Serial.println(fault_type_name(fault_current_type()));`.

**Unsafe ISRs.**
The original GPIO ISRs (`handlePMMFault`, `handleHighVoltagePhaseFault`) ran the entire fault response — `Serial.println`, `delay(500)`, busy-wait recovery loops with serial command processing — directly inside the interrupt context. Fix (Stage 3): ISRs were reduced to "ack the IRQ, force the output stage off via `pwm_disable_output_stage()`, set `pending = true` and the fault type, return". All blocking work moved to `fault_handle()` running from the main loop.

**Redundant `gpio_set_function(..., GPIO_FUNC_PWM)` calls.**
The output-stage PWM pin functions were configured twice — once by `setup()` and once again inside the PWM-setup helper. Harmless but misleading; the second set was the authoritative one. Fix (Stage 4): kept only the call in `pwm_setup_output_stage()`.

### 2.3 Dead and obsolete code

**Buck-converter remnants.**
The hardware was at one point a buck-stage design; the buck stage was replaced by a pi-filter module before this branch. The firmware still defined `BUCK_POWER_GOOD` / `BUCK_ENABLE` / `BUCK_ADJ_PWM` pins, configured them in `setup()`, drove them low, and registered ISRs against `BUCK_POWER_GOOD` for "high current phase fault" (which the pi-filter module cannot raise — it has no PGOOD line). Fix (Stage 4): removed the pins, the fault enum, and the dead ISR.

**`*_IN_USE` flags that were always one constant.**
`PMM_FAULT_IN_USE` was commented "should always be true". `HIGH_CURRENT_PGOOD_IN_USE` was always false because the pi-filter has no PGOOD. `HIGH_VOLTAGE_PGOOD_IN_USE` was always true. Fix: deleted, with the conditional ISR registration replaced by direct unconditional attaches.

**`oldDeviceState` shadow variable.**
Used to "restore" `deviceState` after a fault, but the value being restored was always either `OPERATING` or `IDLE`, both of which are re-derived from `EDM_ENABLE` on the next peripheral tick anyway. Fix (Stage 5): set `deviceState = IDLE` after fault recovery and let the next peripheral tick sort it out. This also gives a defined, safe post-fault state instead of "whatever happened to be in the shadow at trip time".

**`PERIPHERAL_MANAGEMENT` as a device state.**
The original `loop()` would set `deviceState = PERIPHERAL_MANAGEMENT` every 10 ms, do periodic work, then restore the previous state. Fix (Stage 5): periodic work is now just a periodic function call from `loop()`. There is no state to enter and leave.

**`enablePortStatus` global flag.**
A `volatile bool` updated by one function and read by one other, both running on the main loop with no ISR touching it. Replaced with a single `digitalRead(EDM_ENABLE)` at the point of use.

### 2.4 Style and convention issues
The original was a single 1530-line file with mixed `camelCase`/`PascalCase` function names, occasional Allman-style braces, and inline comments dominated by paraphrasing of the code on the next line. Stage 3 onwards adopted CLAUDE.md conventions: `module_func_desc` for functions, `SCREAMING_SNAKE_CASE` for constants, K&R braces, and comments that explain *why* (hardware quirks, polarity inversions, shoot-through prevention) rather than *what*.

---

## 3. Theory of Operation

### 3.1 Hardware overview
The Powercore is a two-phase EDM power supply:
- **High-voltage phase** ignites the discharge across the wire/workpiece gap. A boost-converter module produces 64–100 V DC, set by a TPL0401B digital potentiometer over I2C.
- **High-current phase** delivers the bulk discharge energy through a pi-filter module. There is no programmable rail here — the filter draws from the 48 V input.
- Four MOSFETs (`SW_ENABLE`, `SW_HIGH_CURRENT_PHASE`, `SW_HIGH_VOLTAGE_PHASE`, plus the comparator-fed `OUTPUT_OVERCURRENT_SET`) are gated by RP2040 PWM. `SW_HIGH_VOLTAGE_PHASE` and `SW_HIGH_CURRENT_PHASE` are P-channel and therefore inverted; `SW_ENABLE` is N-channel.
- A motion controller (typically LinuxCNC) toggles `EDM_ENABLE` to request machining and reads back a power-ratio PWM on `EDM_FEEDBACK`.
- Three ADC channels: `PMM_ISENSE` (input current, 200 mV/A), `OUTPUT_VSENSE` (output voltage via 99.6:1 divider), `OUTPUT_ISENSE` (output current, TMCS1133 25 mV/A).
- Three fault sources: `PMM_FAULT` (active-low), `BOOST_PGOOD` (active-low), `OUTPUT_OVERCURRENT` (comparator, falling edge).

### 3.2 Module map and ownership
```
firmware.ino                          device-level state, setup, loop, mode bodies
├── config.h                          pin map, hardware constants, SOA limits
└── src/
    ├── types.h                       DeviceState, ModeOfOperation, FaultStateType, structs
    ├── sensors.{h,cpp}               ADC, calibration, running average, conversions
    ├── pwm_control.{h,cpp}           feedback PWM, output-stage PWM, safe-OFF
    ├── boost_module.{h,cpp}          DPOT I2C, voltage table, target-voltage ramp
    ├── fault_handler.{h,cpp}         pending/active fault, ISRs, main-loop dispatch
    └── telemetry.{h,cpp}             Serial, command parser, status block, validation
```

Globals owned by `firmware.ino` (`deviceState`, `modeOfOperation`, machining parameters, discharge stats, `edgeDetected`) are read by `telemetry.cpp` via `extern` declarations at the top of that file. This is deliberate: it keeps device-level state in one place without introducing a "globals header" that every module would have to include.

### 3.3 Startup
`setup()` runs through a fixed sequence:
1. `deviceState = STARTUP`.
2. Pin modes and initial pin states (PMM disabled, diag-EN asserted).
3. ADC at 12-bit resolution.
4. Feedback PWM configured at 1 kHz with a /10 clock divider; output stage forced to safe-OFF.
5. **PMM zero-current calibration** with the high-side switch still off — averages 100 ADC samples to capture the sensor's zero offset.
6. PMM high-side switch enabled; 500 ms inrush-settling spin.
7. **Output-current zero calibration** — same idea, 100 samples.
8. Serial up at 115200 baud, version banner.
9. Fault ISRs attached (PMM, BOOST_PGOOD); output-overcurrent ISR attached (dispatches by mode).
10. I2C up; **boost voltage table built** by sweeping the DPOT 0→110, recording averaged output voltage at each position. If the DPOT does not echo back its written value, or if the maximum measured voltage cannot reach `MAX_HIGH_VOLTAGE_PHASE_VOLTS` (100 V), `fault_trip(HIGH_VOLTAGE_PHASE_SETUP_FAULT)` is called and startup completes with the device latched into the fault path.
11. `deviceState = IDLE`.

### 3.4 Main loop
```
loop():
    now = millis()

    if fault_pending:
        deviceState = FAULT
        fault_handle()       ← blocks until recovery (or forever, for non-recoverable)
        deviceState = IDLE

    every peripheralManagementInterval_MS (10 ms):
        handle_peripheral_management()

    switch deviceState:
        OPERATING → handle_operating_state()
        IDLE      → handle_idle_state()

    every periodicTelemetryInterval_MS (1 s):
        telemetry_send()
```

`handle_peripheral_management()` does the housekeeping: pushes one PMM sample into the running-average buffer, checks input current against `MAX_SAFE_INPUT_CURRENT` (trips `POWER_OUT_OF_RANGE_FAULT` if exceeded), reads `EDM_ENABLE` to set `deviceState`, updates the `EDM_FEEDBACK` duty cycle proportional to current input power (active-low: full duty = no power), and drains one line of serial input if any is queued.

### 3.5 Operating modes
**EDM Iso-frequency mode** — constant-frequency discharge generation.

`pwm_setup_output_stage()` configures all four output-stage PWMs at the requested machining frequency in **phase-correct mode** (counter counts up then down, so the period is doubled and `wrap = f_clk / (f_out * 2)`). The HV-phase counter is offset by `EDM_ISOFREQ_HV_PWM_OFFSET` (0.25 of wrap) at startup to prevent shoot-through between `SW_HIGH_VOLTAGE_PHASE` and `OUTPUT_OVERCURRENT_SET`, which share a slice. Polarity is inverted on the slices that drive P-channel MOSFETs.

The output-overcurrent comparator threshold is set by `OUTPUT_OVERCURRENT_SET` at a duty whose averaged voltage corresponds to the desired current limit (`v_thresh = (ISENSE_OUTPUT_V_PER_AMP * current_limit_A) / 2.5`).

When the comparator fires (a discharge has occurred), the overcurrent ISR captures `analogRead(OUTPUT_ISENSE)` and `analogRead(OUTPUT_VSENSE)`, increments the discharge counters, sets `newDischargeDetected`, and acks the IRQ. The main-loop body of `edm_isofreq_mode()` then folds those samples into exponential moving averages (`avgDischargeCurrent`, `avgDischargeVoltage`) and computes a windowed success rate every `DISCHARGES_PER_CALC_INTERVAL` (10) discharges or every `dischargeRateCalculationInterval_MICROS`, whichever comes first.

Two pulse-skip protections run on every interval:
- If the windowed success rate exceeds `MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD` (0.8), the output stage is disabled for 250 ms — too many successful ignitions in a row implies the wire/workpiece gap has shorted.
- If averaged input power exceeds `MAX_INPUT_POWER_SETPOINT_WATTS` (75 W), the output stage is disabled for 10 ms — back off until the input current settles.

If the operator requested a finite `dischargeCountTarget` and that count has been reached, the output stage shuts down and `requestedDischargesReached` latches. The latch clears the next time the operator changes parameters (`apply_parameters()` resets `modePreparationComplete`, which causes `setup_iso_output_pwm()` to run again).

**Edge-detection mode** — single-pulse workpiece probing.

The output stage is configured at minimum HV-phase voltage (64 V), 5% duty, 10 kHz, with the high-current phase disabled. The first overcurrent event sets `edgeDetected`, disables the output, drives feedback to 100% duty for 1 s as a signal to the host, then back to 0% for 1 s, and clears the latch. The flag is also surfaced in the telemetry block.

### 3.6 Fault model
Three sources of trips:
1. **PMM fault** (recoverable). ISR acks the IRQ and calls `fault_trip(PMM_FAULT_TYPE)`. The main-loop handler blinks the status LED off for 500 ms and clears.
2. **Boost PGOOD low** (recoverable). Same ISR pattern; the main-loop handler holds in `service_serial_until(boost_recovered)`, processing serial commands while waiting for `BOOST_PGOOD` to go high again.
3. **Output overcurrent during boost-table build** (`HIGH_VOLTAGE_PHASE_SETUP_FAULT`) and **input over-power** (`POWER_OUT_OF_RANGE_FAULT`). Non-recoverable: the handler enters `service_serial_until(NULL)`, accepting only `RESET_DEVICE` (which calls `rp2040.reboot()`).

Every entry into the fault path force-disables the output stage *first* (in the ISR, before the main loop sees the trip flag), so the dispatcher can take its time without leaving the FETs in an indeterminate state.

### 3.7 Serial command surface
Commands are newline-terminated, dispatched by `telemetry_process_command()`:
- `SEND_TELEMETRY` — print the multi-line status block (firmware version, device state, fault, input current/power, mode-specific stats, parameters)
- `SET_ALL_PARAMETERS <discharges> <duty> <freq> <init_v>` — validates against `config.h` SOA limits including computed on-time / off-time, applies if valid, clears `modePreparationComplete` so the next operating tick reconfigures PWM
- `EDGE_DETECTION_MODE` / `EDM_ISOFREQUENCY_MODE` — switch operating mode and force a reconfigure
- `RESET_DEVICE` — soft reboot via `rp2040.reboot()`
- `SET_DPOT <pos>` — direct DPOT write (development/diagnostic)
- `READ_HVP_VOLTAGE` — averaged read of boost output voltage
- `UPDATE_DPOT_VOLTAGE_TABLE` — re-run the startup voltage-table sweep
- `SET_DPOT_FROM_VTABLE <volts>` — ramp DPOT to closest table entry
- `SET_FEEDBACK_DUTY <0..1>` — directly set the EDM_FEEDBACK duty (development/diagnostic)

Unknown commands print a help summary.

---

## 4. Verification at end of refactor
- Build: `arduino-cli compile --fqbn rp2040:rp2040:rpipico --warnings all` — clean, no warnings.
- Size: 74820 B flash (3% of 2093056), 10244 B RAM (3% of 262144).
- Static checks: removed-identifier sweep returns hits only in `notes.md`, `functional-decomp.md`, `stale_firmware.txt`. No live-code regressions.
- Git: each stage is its own commit on `firmware-refactor`, bisectable.

What remains untested without hardware in the loop: end-to-end machining behavior in iso-frequency mode, edge-detect latch timing, fault recovery on a real boost module. Recommend a bench-test pass before merging to `main`.
