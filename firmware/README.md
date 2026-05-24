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

# Theory of Operation

### Hardware overview
The Powercore is a two-phase EDM power supply:
- **High-voltage phase** ignites the discharge across the wire/workpiece gap. A boost-converter module produces 64–100 V DC, set by a TPL0401B digital potentiometer over I2C.
- **High-current phase** delivers the bulk discharge energy through a pi-filter module. There is no programmable rail here — the filter draws from the 48 V input.
- Four MOSFETs (`SW_ENABLE_PIN`, `SW_HIGH_CURRENT_PHASE_PIN`, `SW_HIGH_VOLTAGE_PHASE_PIN`, plus the comparator-fed `OUTPUT_OVERCURRENT_SET_PIN`) are gated by RP2040 PWM. `SW_HIGH_VOLTAGE_PHASE_PIN` and `SW_HIGH_CURRENT_PHASE_PIN` are P-channel and therefore inverted; `SW_ENABLE_PIN` is N-channel.
- A motion controller (typically LinuxCNC) toggles `EDM_ENABLE_PIN` to request machining and reads back a power-ratio signal on `EDM_FEEDBACK_PIN`, encoded as PWM frequency at a fixed duty cycle (configurable range, default 50–500 Hz).
- Three ADC channels: `PMM_ISENSE_PIN` (input current, 200 mV/A), `OUTPUT_VSENSE_PIN` (output voltage via 99.6:1 divider), `OUTPUT_ISENSE_PIN` (output current, TMCS1133 25 mV/A).
- Three fault sources: `PMM_FAULT_PIN` (active-low), `BOOST_PGOOD_PIN` (active-low), `OUTPUT_OVERCURRENT_PIN` (comparator, falling edge).

### Module map and ownership
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

`main.c` owns three things: `g_ctx` (the runtime context), `g_boost_dpot` (DPOT handle), and `g_boost_cal` (cal table). Every other module is a singleton with file-static state inside its own `.c`. Cross-module communication is by passing a `main_ctx_t *` pointer; only ISR-shared fields are individually `volatile`.

### Startup
`main()` runs through a fixed sequence:
1. `ctx_init(&g_ctx)` — state = STARTUP, default machining parameters, copy feature flags from `config.h`.
2. `cmd_init()` — `stdio_init_all()` brings up USB-CDC; 1 s blocking enumeration delay so early prints land in a connected terminal.
3. STATUS LED, EDM_ENABLE GPIOs, `output_init()` (PWM slice cache + EDM_FEEDBACK PWM + safe-OFF), `pmm_init()`.
4. `sensors_init()` — adc_init + adc_gpio_init for the three sensor channels.
5. Boost DPOT created and I2C wired; `cmd_set_boost_ctx()` injects the DPOT and cal table into the cmd module so `SET_DPOT` etc. work.
6. **PMM zero-current calibration** with the high-side switch still off.
7. PMM enable; `PMM_INRUSH_DELAY_MS` (500 ms) inrush spin.
8. **Output-current zero calibration** — same idea.
9. Single GPIO IRQ callback registered (`gpio_irq_dispatch` in `main.c`); `IO_IRQ_BANK0` enabled.
10. `fault_init() + fault_attach_irqs()` — arms PMM_FAULT and BOOST_PGOOD edge IRQs.
11. `output_attach_isr(&g_ctx)` — binds ctx for the overcurrent ISR, arms OUTPUT_OVERCURRENT.
12. **HV-only output stage** brought up so the boost can be characterised.
13. **Boost voltage table built** by sweeping the DPOT 0→`BOOST_DPOT_POSITION_MAX` and recording averaged HV-rail readings. Failure (timeout, missing DPOT) leaves `cal->calibrated == false`.
14. **Max-safe HV check** (`verify_boost_cal_safe`) — scans the cal table for the highest measured voltage and trips `HIGH_VOLTAGE_PHASE_SETUP_FAULT` if it exceeds `MAX_HIGH_VOLTAGE_PHASE_VOLTS`, or if the table never got built.
15. Output stage disabled. If a fault is pending, print "ERROR: setup failed; fault pending" and let iteration 1 of the loop dispatch it (non-recoverable). Otherwise STATUS_LED on, "OK: Setup complete", state → IDLE.

### Main loop
```
while (1):
    now = to_ms_since_boot(get_absolute_time())

    if fault_pending():                       (1) faults take priority
        ctx_set_state(FAULT)
        fault_handle(&g_ctx, cmd_poll)        ← blocks until recovery
                                                (operator commands still serviced)
        ctx_reset_discharge_stats()
        ctx_set_state(IDLE)
        continue

    cmd_poll(&g_ctx)                          (2) commands every iteration

    if now - last_periph_tick >= PERIPH_MGMT_INTERVAL_MS:   (3) housekeeping @ 100 Hz
        sensors_sample_input_current()
        if avg_input > MAX_SAFE_INPUT_CURRENT: fault_trip(POWER_OUT_OF_RANGE_FAULT)
        state ← gpio_get(EDM_ENABLE_PIN) ? OPERATING : IDLE
        OPERATING: output_feedback_set_ratio(input_power / MAX_INPUT_POWER_SETPOINT_WATTS)
        IDLE:      output_feedback_disable()

    switch state:                             (4) state body
        OPERATING: lazy mode prep + output_run_isofreq / output_run_edge
        IDLE:      output_stage_disable + clear mode_prep_done; sleep_ms(10)

    cmd_tick(&g_ctx)                          (5) periodic telemetry @ 1 Hz
```

Mode preparation happens at OPERATING entry (`prepare_isofreq_mode` / `prepare_edge_mode`), not lazily inside the per-tick body. The caller (main.c) is responsible for ramping the boost via `boost_set_voltage()` *before* calling `output_setup_isofreq` / `output_setup_edge`; the output module only validates `cal->calibrated`.

### Operating modes
**EDM Iso-frequency mode** — constant-frequency discharge generation.

`output_stage_setup()` configures all four output-stage PWMs at the requested machining frequency in **phase-correct mode** (counter counts up then down, so the period is doubled and `wrap = PWM_BASE_CLOCK_FREQ / (frequency_hz * 2)`). The HV-phase counter is offset by `EDM_ISOFREQ_HV_PWM_OFFSET` (0.25 of wrap) at startup to prevent shoot-through between `SW_HIGH_VOLTAGE_PHASE_PIN` and `OUTPUT_OVERCURRENT_SET_PIN`, which share PWM slice 4. `SW_ENABLE_PIN` and `SW_HIGH_CURRENT_PHASE_PIN` share slice 5; they cannot be enabled independently. Polarity is inverted on the slices that drive P-channel MOSFETs.

When the comparator fires (a discharge has occurred), `output_overcurrent_isr()` captures the output current/voltage ADC values into `ctx->new_discharge_*_adc`, increments counters, and sets `ctx->new_discharge_detected`. `output_run_isofreq()` then folds those samples into exponential moving averages (`ctx->avg_discharge_current/voltage`) and computes a windowed success rate every `DISCHARGES_PER_CALC_INTERVAL` (10) discharges or every `ctx->discharge_rate_calc_interval_us`, whichever comes first.

Two pulse-skip protections run on every interval (gated by `ctx->allow_success_rate_pulse_skip` and `ctx->allow_power_setpoint_pulse_skip`):
- If the windowed success rate exceeds `MAX_DISCHARGE_SUCCESS_RATE_THRESHOLD` (0.8), the output stage is disabled for 250 ms.
- If averaged input power exceeds `MAX_INPUT_POWER_SETPOINT_WATTS` (75 W), the output stage is disabled for 10 ms.

After each pulse-skip, an internal helper re-arms the PWM slices with the current params — the original Arduino-era code disabled but never re-enabled, leaving the output dead until the next IDLE cycle.

If the operator requested a finite `params.discharge_count_target` and that count has been reached, the output stage shuts down and `requested_discharges_reached` latches. The latch clears on the next `SET_ALL_PARAMETERS` (which clears `mode_prep_done`).

**Edge-detection mode** — single-pulse workpiece probing.

`output_setup_edge()` configures the main slice (`SW_ENABLE_PIN` + `SW_HIGH_CURRENT_PHASE_PIN`) at 5%/10 kHz with the HV slice off, after ramping the boost to `MIN_HIGH_VOLTAGE_PHASE_VOLTS`. The first overcurrent event sets `ctx->edge_detected`, `output_overcurrent_isr` disables the stage immediately, and `output_run_edge()` drives feedback to `EDM_FEEDBACK_FREQ_MAX_HZ` for 1 s as a signal to the host, then disables the PWM (line idles low) for 1 s, and clears the latch.

### Fault model
Three sources of trips:
1. **PMM fault** (recoverable). `fault_pmm_isr` → `fault_trip(PMM_FAULT_TYPE)`. Main-loop handler blinks the status LED off for 500 ms and clears.
2. **Boost PGOOD low** (recoverable). `fault_boost_pgood_isr` → `fault_trip(BOOST_CONVERTER_PGOOD_FAULT)`. Main-loop handler holds in `fault_spin_until(boost_recovered)`, calling `cmd_poll` while waiting for `BOOST_PGOOD_PIN` to go high again.
3. **In-line trips** (`HIGH_VOLTAGE_PHASE_SETUP_FAULT`, `POWER_OUT_OF_RANGE_FAULT`). Non-recoverable: `fault_spin_until(NULL)`, accepting only `RESET_DEVICE` (which calls `watchdog_reboot(0,0,0)`).

`fault_trip()` immediately disables the output stage *before* setting the pending flag, so the device is safe regardless of when `fault_handle()` runs. The RP2040 only allows one system-wide GPIO IRQ callback; `gpio_irq_dispatch` in `main.c` owns it and dispatches by pin to `fault_pmm_isr` / `fault_boost_pgood_isr` / `output_overcurrent_isr`.

### Command surface
USB-CDC line interface, exact-match dispatch, strict argc validation, `OK:` / `ERROR:` response prefixes:

| Command | Effect |
|---|---|
| `SEND_TELEMETRY` | Print the multi-line status block immediately |
| `SET_ALL_PARAMETERS <discharges> <duty> <freq> <init_v>` | Validate against `config.h` SOA limits including computed on/off times, commit and clear `mode_prep_done` so the next OPERATING entry reconfigures PWM |
| `EDGE_DETECTION_MODE` / `EDM_ISOFREQUENCY_MODE` | Disable output stage, switch mode |
| `RESET_DEVICE` | Disable output stage, drain stdio, `watchdog_reboot(0,0,0)` |
| `SET_DPOT <pos>` | Direct DPOT write (development/diagnostic) |
| `READ_HVP_VOLTAGE` | Averaged read of boost output voltage |
| `UPDATE_DPOT_VOLTAGE_TABLE` | Re-run the startup voltage-table sweep |
| `SET_DPOT_FROM_VTABLE <volts>` | Ramp DPOT to closest table entry |
| `SET_FEEDBACK_RATIO <0..1>` | Directly set the EDM_FEEDBACK power ratio (encoded as PWM frequency) (development/diagnostic) |
| `HELP [<command>]` | List all commands, or print full usage for one |

Unknown commands print `ERROR: Unknown command 'X'` followed by the full HELP listing.

---
