# Setup
This project requires Earle Phillhower's Arduino-Pico (RP2040) Core
If using the `arduino-cli`...

Install the RP2040 Core
```bash
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index
arduino-cli core install rp2040:rp2040
```

## Flashing
This firmware may be flashed to the RP2040 using the Arduino IDE, or using the `arduino-cli`:

To identify the correct port:
```bash
arduino-cli board list
```

Check for the port name, something like `/dev/ttyACM0` if on Linux. You may unplug the device and re-run the command to check it's the correct port. Then upload the firmware:
```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico ./firmware.ino
arduino-cli upload -p <YOUR_PORT> --fqbn rp2040:rp2040:rpipico ./firmware.ino
```

# Theory of Operation

### Hardware overview
The Powercore is a two-phase EDM power supply:
- **High-voltage phase** ignites the discharge across the wire/workpiece gap. A boost-converter module produces 64–100 V DC, set by a TPL0401B digital potentiometer over I2C.
- **High-current phase** delivers the bulk discharge energy through a pi-filter module. There is no programmable rail here — the filter draws from the 48 V input.
- Four MOSFETs (`SW_ENABLE`, `SW_HIGH_CURRENT_PHASE`, `SW_HIGH_VOLTAGE_PHASE`, plus the comparator-fed `OUTPUT_OVERCURRENT_SET`) are gated by RP2040 PWM. `SW_HIGH_VOLTAGE_PHASE` and `SW_HIGH_CURRENT_PHASE` are P-channel and therefore inverted; `SW_ENABLE` is N-channel.
- A motion controller (typically LinuxCNC) toggles `EDM_ENABLE` to request machining and reads back a power-ratio PWM on `EDM_FEEDBACK`.
- Three ADC channels: `PMM_ISENSE` (input current, 200 mV/A), `OUTPUT_VSENSE` (output voltage via 99.6:1 divider), `OUTPUT_ISENSE` (output current, TMCS1133 25 mV/A).
- Three fault sources: `PMM_FAULT` (active-low), `BOOST_PGOOD` (active-low), `OUTPUT_OVERCURRENT` (comparator, falling edge).

### Module map and ownership
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

### Startup
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

### Main loop
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

### Operating modes
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

### Fault model
Three sources of trips:
1. **PMM fault** (recoverable). ISR acks the IRQ and calls `fault_trip(PMM_FAULT_TYPE)`. The main-loop handler blinks the status LED off for 500 ms and clears.
2. **Boost PGOOD low** (recoverable). Same ISR pattern; the main-loop handler holds in `service_serial_until(boost_recovered)`, processing serial commands while waiting for `BOOST_PGOOD` to go high again.
3. **Output overcurrent during boost-table build** (`HIGH_VOLTAGE_PHASE_SETUP_FAULT`) and **input over-power** (`POWER_OUT_OF_RANGE_FAULT`). Non-recoverable: the handler enters `service_serial_until(NULL)`, accepting only `RESET_DEVICE` (which calls `rp2040.reboot()`).

Every entry into the fault path force-disables the output stage *first* (in the ISR, before the main loop sees the trip flag), so the dispatcher can take its time without leaving the FETs in an indeterminate state.

### Serial command surface
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
