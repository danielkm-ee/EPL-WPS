# Firmware Dependency Review

## Overview

The firmware is a mixed Arduino + bare-metal sketch: it runs inside the arduino-pico (earle-philhower) environment but bypasses most of the Arduino hardware abstractions in favour of direct RP2040 Pico SDK calls, especially for PWM, GPIO, and I2C.

---

## 1. Standard Arduino Core

Used pervasively in `firmware.ino` and `tpl0401b_test.ino`. All of these are part of the standard Arduino API and are supported by any board package.

| API | Where used |
|---|---|
| `Serial` (begin, available, readStringUntil, print, println) | Serial command interface and telemetry output |
| `millis()` / `micros()` | State-machine timeouts, duty-cycle timing |
| `delay()` / `delayMicroseconds()` | Initialisation sequences, short spin-waits |
| `pinMode()` / `digitalRead()` | Input setup and EDM enable pin polling |
| `analogRead()` / `analogReadResolution()` | ADC reads for current/voltage sense and PMM |
| `attachInterrupt()` / `digitalPinToInterrupt()` | FALLING-edge ISRs on four GPIO pins |
| `String` class (trim, substring, toInt, toFloat, startsWith, indexOf) | Serial command parsing |

`setup()` / `loop()` entry points are used as expected.

---

## 2. RP2040 Pico SDK — Direct Usage (Heavy)

The firmware includes the Pico SDK hardware headers directly and calls their APIs for everything timing- and signal-critical. **This is the dominant hardware interface in the codebase.**

```cpp
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"
```

### GPIO (`hardware/gpio.h`)
- `gpio_set_function(pin, GPIO_FUNC_PWM / GPIO_FUNC_I2C)` — mux pins to PWM and I2C peripherals
- `gpio_put()` / `gpio_get()` — direct pin writes/reads (used instead of `digitalWrite`/`digitalRead` in most places)
- `gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_FALL)` — explicit IRQ clearing inside ISRs

### PWM (`hardware/pwm.h`)
The entire EDM pulse generation is built on the Pico SDK PWM API. No Arduino analogWrite abstraction is used.
- `pwm_gpio_to_slice_num()` / `pwm_gpio_to_channel()` — slice/channel resolution
- `pwm_set_wrap()` / `pwm_set_clkdiv_int_frac()` — period and clock divider
- `pwm_set_phase_correct()` — phase-correct mode for symmetric dead-time
- `pwm_set_output_polarity()` — invert channels for complementary switching
- `pwm_set_chan_level()` — duty cycle
- `pwm_set_counter()` / `pwm_set_enabled()` — synchronised start/stop of slices

### I2C (`hardware/i2c.h`)
DPOT (TPL0401B at address `0x3E`) is driven with blocking Pico SDK calls:
- `i2c_init(i2c0, 10000)` — 10 kHz bus
- `i2c_write_blocking()` / `i2c_read_blocking()` — register writes and reads

`tpl0401b_test.ino` uses the Arduino `Wire` library instead (see §3).

---

## 3. earle-philhower arduino-pico Specifics

These are non-standard additions provided by the earle-philhower board package — they are not part of the standard Arduino core or the bare Pico SDK.

| API | Location | Purpose |
|---|---|---|
| `rp2040.reboot()` | `firmware.ino` | Executes a soft reboot in response to the `RESET_DEVICE` serial command |
| `Wire.setSDA()` / `Wire.setSCL()` | `tpl0401b_test.ino` | Pin-remappable I2C (RP2040-specific Wire extension) |
| `Wire.begin()` / `Wire.setClock()` / `Wire.beginTransmission()` / `Wire.write()` / `Wire.endTransmission()` / `Wire.requestFrom()` / `Wire.read()` | `tpl0401b_test.ino` | Arduino Wire I2C for the standalone DPOT test sketch |

The main firmware does **not** use `Wire`; it calls the Pico SDK I2C API directly. The test sketch uses `Wire` for simplicity.

---

## 4. Third-Party Libraries

None. The only `#include` beyond stdlib, Pico SDK, and the built-in `Wire.h` is the standard C headers (`stdio.h`, `string.h`, `stdlib.h`, `ctype.h`).

---

## 5. Dependency Depth Assessment

| Layer | Depth |
|---|---|
| **earle-philhower arduino-pico** | **Shallow** — only `rp2040.reboot()` and the `Wire` library (test sketch only). Swapping board packages would require replacing one reboot call and the test sketch's I2C setup. |
| **Arduino core (Serial, timing, analogRead, interrupts, String)** | **Moderate** — used for the serial interface, timing, ADC, and interrupt wiring. Replacing these would require ~50–100 call sites but no architectural change. |
| **RP2040 Pico SDK (PWM, GPIO, I2C)** | **Deep** — the entire pulse-generation engine and DPOT control are built directly on Pico SDK primitives. This code is not portable to other microcontroller families without significant rewriting. |

---

## 6. Key Takeaways

- The firmware is **not easily portable** to non-RP2040 hardware because PWM phase-correct configuration, GPIO mux, and synchronised slice start/stop are all expressed in Pico SDK terms.
- The Arduino layer is thin and could, in principle, be replaced with a bare `main()` loop using `pico/stdlib.h` timers. The main value it provides today is `Serial`, `analogRead`, and interrupt glue.
- The planned `src/` refactor should keep the Pico SDK calls (they are deliberate), encapsulate them in the module headers (`pwm_control`, `boost_module`, `sensors`), and retire the Arduino String parsing in favour of `sscanf` / `strcmp` to reduce heap allocation.
