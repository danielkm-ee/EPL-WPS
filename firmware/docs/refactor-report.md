# Firmware Refactor Report

**Branch:** `firmware-refactor`
**Baseline:** `firmware.ino` — 1530 lines, single file, would not compile
**Result:** `firmware.ino` (336 lines) + 5 modules under `src/` (~900 lines total), clean compile at 74820 B flash / 10244 B RAM

**Goals:** make the firmware compile, fix latent runtime bugs, split into maintainable modules, and remove dead code from a prior hardware revision.

---

## Changelog

### Stage 1 — Bug fixes
- Fixed missing closing brace in `READ_HVP_VOLTAGE()` that caused the entire file to fail parsing
- Renamed `volatile FaultStateType FaultStateType` (variable shadowing its own type) to `currentFaultType`
- Fixed HV fault ISRs acknowledging `BOOST_CONVERTER_PGOOD_FAULT` (a fault enum value, not a GPIO) instead of `BOOST_PGOOD` (GPIO 18) — the interrupt was never actually cleared
- Fixed `sendTelemetry()` always printing `FAULT NONE` due to `isInFaultState` being declared but never assigned; replaced with `deviceState == FAULT`
- Moved all blocking work (`Serial.println`, `delay`, recovery loops) out of ISR context into `fault_handle()` on the main loop; ISRs now only ack the IRQ, disable the output stage, and set a pending flag

### Stage 2 — Extract shared headers
- `config.h`: all pin assignments, hardware constants, scaling factors, and safe-operating limits
- `src/types.h`: all shared enums and structs (`DeviceState`, `ModeOfOperation`, `FaultStateType`, `OutputParameters`, `PWMOutput`)

### Stage 3 — Split into modules

| Module | Responsibility |
|---|---|
| `sensors` | ADC setup, calibration, input-current averaging, ADC→engineering-unit conversions |
| `pwm_control` | EDM-feedback PWM and output-stage PWM setup, safe-OFF disable |
| `boost_module` | TPL0401B DPOT over I2C, voltage lookup table, `boost_set_voltage` ramping |
| `fault_handler` | Fault state, ISR registration, main-loop `fault_handle()` dispatcher |
| `telemetry` | Serial lifecycle, command parser, telemetry formatter, parameter validation |

### Stage 4 — Remove dead code (buck-converter era)
- Removed `BUCK_POWER_GOOD`, `BUCK_ENABLE`, `BUCK_ADJ_PWM` pin definitions and their `setup()` configuration (buck stage replaced by pi-filter board)
- Removed `BUCK_CONVERTER_PGOOD_FAULT` enum and its dead ISR
- Removed `PMM_FAULT_IN_USE`, `HIGH_CURRENT_PGOOD_IN_USE`, `HIGH_VOLTAGE_PGOOD_IN_USE` flags (all were compile-time constants); replaced conditional ISR registration with unconditional attaches
- Removed unused `pmm_recovered()` helper
- Removed duplicate `gpio_set_function(..., GPIO_FUNC_PWM)` calls
- Promoted two magic numbers to named `config.h` constants

### Stage 5 — Simplify state machine
- Removed `PERIPHERAL_MANAGEMENT` device state; periodic work is now an unconditional timed call from `loop()`
- Removed `oldDeviceState` shadow; post-fault recovery sets `deviceState = IDLE` and the next peripheral tick re-derives `OPERATING`/`IDLE` from `EDM_ENABLE`
- Removed global `currentFaultType`; owned internally by `fault_handler`, queried via `fault_current_type()`
- Inlined `enablePortStatus` global flag into a direct `digitalRead(EDM_ENABLE)` at the call site

---

