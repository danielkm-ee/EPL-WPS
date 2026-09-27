# Setup
This firmware is a pure C11 library against the Raspberry Pi Pico SDK.
No Arduino-Pico core is required.

## Toolchain (Fedora 43)
```bash
sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++ \
                 arm-none-eabi-newlib cmake make libusb1-devel
```

## Pico SDK
Install the SDK anywhere; this project assumes `~/.local/share/pico-sdk`.
Initialise its submodules so TinyUSB (and friends) are present:
```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/.local/share/pico-sdk
cd ~/.local/share/pico-sdk && git submodule update --init
```

## picotool (USB flash + introspection)
The SDK's bundled picotool is built without USB support. Build a
standalone copy once:
```bash
git clone https://github.com/raspberrypi/picotool.git ~/.local/share/picotool-src
cd ~/.local/share/picotool-src && PICO_SDK_PATH=$HOME/.local/share/pico-sdk cmake -B build && make -C build -j
```

For non-root USB access, install the udev rule once:
```bash
sudo cp ~/.local/share/picotool-src/udev/60-picotool.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## Build
```bash
cd firmware && mkdir build && cd build
PICO_SDK_PATH=$HOME/.local/share/pico-sdk cmake .. \
    -Dpicotool_DIR=$HOME/.local/share/picotool-src/install/lib/cmake/picotool
make -j
```
Artifacts land in `firmware/build/`: `epl_wps.elf`, `epl_wps.bin`,
`epl_wps.uf2`.

## Flash
With the Pico held in BOOTSEL (or already running compatible firmware
that exposes the picotool reset interface):
```bash
~/.local/share/picotool-src/build/picotool load -x firmware/build/epl_wps.uf2
```
`-x` reboots into the application after the load completes.

## Connect
The device exposes USB-CDC at `/dev/ttyACM0` (baud rate is irrelevant;
USB-CDC ignores it):
```bash
screen /dev/ttyACM0 115200      # or: minicom -D /dev/ttyACM0 -b 115200
```
Type `HELP` for the command listing, `HELP <cmd>` for usage of one.

# Command Interface
USB-CDC line interface, exact-match dispatch, strict argc validation,
`OK:` / `ERROR:` response prefixes:

| Command | Effect |
|---|---|
| `SEND_TELEMETRY` | Print the multi-line status block immediately |
| `SET_TELEMETRY <on/off>` | Enable or disable the periodic 1 Hz telemetry tick |
| `SET_ALL_PARAMETERS <discharges> <duty> <freq> <init_v>` | Validate against `config.h` SOA limits including computed on/off times, commit and clear `mode_prep_done` so the next OPERATING entry reconfigures PWM |
| `EDGE_DETECTION_MODE` / `EDM_ISOFREQUENCY_MODE` | Disable output stage, switch mode |
| `RESET_DEVICE` | Disable output stage, drain stdio, `watchdog_reboot(0,0,0)` |
| `SET_DPOT <pos>` | Direct DPOT write (development/diagnostic) |
| `READ_HVP_VOLTAGE` | Averaged read of boost output voltage |
| `UPDATE_DPOT_VOLTAGE_TABLE` | Re-run the startup voltage-table sweep |
| `SET_DPOT_FROM_VTABLE <volts>` | Ramp DPOT to closest table entry |
| `SET_FEEDBACK_RATIO <0..1>` | Directly set the EDM_FEEDBACK power ratio (encoded as PWM frequency) (development/diagnostic) |
| `HELP [<command>]` | List all commands, or print full usage for one |

Unknown commands print `ERROR: Unknown command 'X'` followed by the full
HELP listing.

# Theory of Operation

### Hardware overview
The Powercore is a two-phase EDM power supply:
- **High-voltage phase** ignites the discharge across the wire/workpiece gap. A boost-converter module produces 64–100 V DC, set by a TPL0401B digital potentiometer over I2C.
- **High-current phase** delivers the bulk discharge energy through a pi-filter module, drawing directly from the 48 V input (no programmable rail).
- Four MOSFETs are gated by RP2040 PWM: `SW_HIGH_VOLTAGE_PHASE_PIN` and `SW_HIGH_CURRENT_PHASE_PIN` are P-channel (inverted polarity); `SW_ENABLE_PIN` is N-channel; `OUTPUT_OVERCURRENT_SET_PIN` sets the comparator threshold.
- A motion controller (typically LinuxCNC) toggles `EDM_ENABLE_PIN` to request machining and reads back a power-ratio signal on `EDM_FEEDBACK_PIN`, encoded as PWM frequency at a fixed 50% duty cycle (200–400 Hz; see `docs/freq-encoding.md`).
- Three ADC channels: `PMM_ISENSE_PIN` (input current), `OUTPUT_VSENSE_PIN` (output voltage), `OUTPUT_ISENSE_PIN` (output current). Scaling constants live in `config.h`.
- Three fault sources: `PMM_FAULT_PIN`, `BOOST_PGOOD_PIN` (both active-low), `OUTPUT_OVERCURRENT_PIN` (comparator, falling edge).

### Architecture
```
main.c                                int main(void), boot sequence, main loop,
                                      single GPIO IRQ dispatcher
├── config.h                          pin map, hardware constants, SOA limits
└── src/
    ├── types.h                       device_state_t, mode_of_operation_t,
    │                                 fault_type_t, output_params_t, pwm_output_t
    ├── ctx.{h,c}                     main_ctx_t — central runtime state
    ├── sensors.{h,c}                 ADC, calibration, running average, conversions
    ├── pmm.{h,c}                     PMM enable + inrush wait, fault polling
    ├── boost.{h,c}                   DPOT I2C, voltage table, target-voltage ramp
    ├── output.{h,c}                  output-stage PWM + feedback + iso/edge mode bodies
    ├── fault.{h,c}                   pending/active fault, ISR entry points,
    │                                 main-loop dispatch
    └── cmd.{h,c}                     stdio_usb, command parser, telemetry block,
                                      static dispatch table
```

`main.c` owns three things: `g_ctx` (the runtime context), `g_boost_dpot`
(DPOT handle), and `g_boost_cal` (cal table). Every other module is a
singleton with file-static state inside its own `.c`. Cross-module
communication is by passing a `main_ctx_t *` pointer; only ISR-shared
fields are individually `volatile`. See `docs/library-reference.md` for
the full module-by-module API.

### Startup
`main()` brings USB-CDC up first so the rest of boot can log, then
initialises GPIOs, ADC, and the boost DPOT/I2C. It calibrates the
input-current zero offset with the PMM switch off, enables the PMM and
waits out inrush, then calibrates the output-current zero offset. It
registers the single system-wide GPIO IRQ callback and arms the
PMM-fault / boost-PGOOD / overcurrent edge IRQs, holds the HV-only
output stage active to sweep the DPOT and build a voltage cal table,
and trips `HIGH_VOLTAGE_PHASE_SETUP_FAULT` if that table failed to
build or measured above the safe ceiling. Boot ends in `IDLE` (or
`FAULT`, handled by the first main-loop iteration).

### Main loop
Each iteration: service any pending fault first (recoverable faults
block in a retry/recovery loop; non-recoverable ones require
`RESET_DEVICE`); poll for operator commands; run a ~100 Hz housekeeping
tick (input-current sampling, the `EDM_ENABLE_PIN` → state transition,
feedback-ratio update); dispatch the current state (`OPERATING` preps
the active mode lazily then runs its per-tick body, `IDLE` disables the
output stage); send periodic telemetry.

### Operating modes
- **EDM iso-frequency** — constant-frequency discharge generation. The
  output stage pulses the HV and HC phases out of phase to avoid
  shoot-through; each discharge event updates running current/voltage
  averages and a windowed success rate, which can pulse-skip the
  output if discharge success or input power runs too high.
- **Edge-detection** — single-pulse workpiece probing at a fixed low
  duty cycle. The first detected discharge disables the output
  immediately and signals the host with a feedback tone burst.

### Fault model
Three trip sources: PMM fault and boost-PGOOD loss are recoverable and
clear automatically once the condition resolves; HV-setup and
input-power faults are non-recoverable and require `RESET_DEVICE`.
`fault_trip()` always disables the output stage before raising the
pending flag, so the hardware is safe regardless of when the main loop
gets around to handling it.

---
