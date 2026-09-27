# feedback_test

Standalone sweep test for the `EDM_FEEDBACK` PWM output. Continuously
sweeps GP3 between 200 Hz and 400 Hz (50% duty cycle) so the signal can
be verified with an oscilloscope or frequency counter. Progress is printed
over USB CDC at 115200 baud.

## Prerequisites

- Pico SDK at `~/.local/share/pico-sdk` (or set `PICO_SDK_PATH` in your
  environment)
- CMake ≥ 3.13 and the `arm-none-eabi-gcc` toolchain
- picotool at `~/.local/share/picotool-src/build/picotool`

Add picotool to your path for convenience:

```bash
export PATH="$HOME/.local/share/picotool-src/build:$PATH"
```

## Build

```bash
cd firmware/feedback_test
mkdir -p build && cd build
PICO_SDK_PATH=~/.local/share/pico-sdk cmake ..
make -j$(nproc)
```

The build produces `feedback_test.uf2` in the `build/` directory.

## Flash

Put the Pico into BOOTSEL mode (hold BOOTSEL while plugging in USB, or
press BOOTSEL + RUN then release RUN), then:

```bash
picotool load -x build/feedback_test.uf2
```

`-x` reboots the device automatically after flashing. If the device is
already running firmware that supports `picotool reboot`, you can skip
the manual BOOTSEL step:

```bash
picotool reboot -f -u   # force reboot into BOOTSEL
picotool load -x build/feedback_test.uf2
```

## Monitor serial output

```bash
minicom -b 115200 -D /dev/ttyACM0
```

Expected output:

```
EDM_FEEDBACK sweep test: GP3, 200 Hz <-> 400 Hz
ratio=0.00  freq=200 Hz
ratio=0.01  freq=202 Hz
...
ratio=1.00  freq=400 Hz
ratio=0.99  freq=398 Hz
...
```
