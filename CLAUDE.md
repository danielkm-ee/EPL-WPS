# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

EPL Wire EDM Wire Power Supply (WPS) — a fork of the Rack Robotics Powercore V3, reworked for a CNC Wire EDM capstone project at the Electronics Prototyping Laboratory. Licensed CC BY-NC-SA 4.0.

**Hardware**: RP2040 (Raspberry Pi Pico) controlling a dual-converter (boost + buck) EDM power supply.

## Building and Flashing Firmware

There is no Makefile or PlatformIO config. Build via the **Arduino IDE**:
1. Install the [arduino-pico](https://github.com/earlephilhower/arduino-pico) board package by Earle Philhower.
2. Open `firmware/firmware.ino` in Arduino IDE.
3. Select board: **Raspberry Pi Pico**.
4. Build and upload via the IDE.

To test the TPL0401B digital potentiometer independently, open `firmware/tpl0401b_test/tpl0401b_test.ino` as a separate sketch.

## Firmware Architecture

The firmware is a monolithic Arduino sketch (`firmware/firmware.ino`, ~1674 lines) currently being refactored into modules under `firmware/src/`. **All files in `firmware/src/` are stubs** — the refactor is in progress. See `firmware/notes.md` for planned changes and known issues.

### State Machine

The device runs a state machine with these states:
- `STARTUP` → `PERIPHERAL_MANAGEMENT` → `IDLE` / `OPERATING` / `FAULT`

Modes of operation: `EDM_ISOFREQUENCY_MODE` (fixed-frequency pulses), `EDGE_DETECTION_MODE` (workpiece probing).

### Key Hardware Interfaces

| Peripheral | GPIO | Notes |
|---|---|---|
| I2C SDA/SCL | 16 / 17 | TPL0401B DPOT at address `0x3E` |
| EDM enable/feedback | 2 / 3 | |
| PWM switches | 8–11 | HV phase, overcurrent set, enable, high-current phase |
| ADC inputs | 26–28 | PMM current sense, output V/I sense |
| Status LED | 25 | Onboard Pico LED |

**TPL0401B DPOT**: controls boost converter output voltage. DPOT positions 0–110 correspond to ~64V–100V. A voltage lookup table in firmware maps DPOT positions to calibrated voltages.

### Serial Interface

115,200 baud. Public commands: `SEND_TELEMETRY`, `SET_ALL_PARAMETERS`, `EDM_ISOFREQUENCY_MODE`, `EDGE_DETECTION_MODE`, `RESET_DEVICE`. Debug-only commands (not documented in README): `SET_DPOT`, `READ_HVP_VOLTAGE`, `UPDATE_DPOT_VOLTAGE_TABLE`, `SET_DPOT_FROM_VTABLE`, `SET_FEEDBACK_DUTY`.

## Repository Layout

```
firmware/           # Arduino firmware (RP2040)
  firmware.ino      # Main sketch — all live code currently lives here
  config.h          # Pin/constant definitions (currently a stub)
  notes.md          # Refactor plan and known code issues
  src/              # Planned modules — ALL STUBS, not yet functional
  tpl0401b_test/    # Standalone I2C DPOT test sketch
circuit-boards/     # KiCAD PCB projects (4 modules)
schematics/         # Schematic PDFs
KiCAD-library/      # Custom KiCAD components and 3D models
LTSpice/            # SPICE simulation files
CAD/                # Mechanical enclosure (.step files)
```

## Planned Refactor (in progress)

`firmware/notes.md` documents the planned modularization into `src/` modules: `boost_module`, `fault_handler`, `pwm_control`, `sensors`, `telemetry`, `types`. When implementing these modules, the arduino-pico plugin is the assumed Arduino core — use its APIs alongside direct RP2040 SDK headers (`pico/stdlib.h`, `hardware/pwm.h`, `hardware/i2c.h`, etc.) where needed.
