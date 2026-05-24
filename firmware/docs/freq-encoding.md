# EDM_FEEDBACK frequency encoding

The `EDM_FEEDBACK` line carries the powercore's instantaneous
input-power-draw ratio to the motion controller, encoded as PWM
frequency at a fixed 50% duty cycle. This document covers:

- what value goes on the wire
- how and how often the firmware updates it
- a LinuxCNC HAL prototype for converting it back into a 0..1 control
  variable on a Mesa 7i76e

## What's on the wire

The transmitted scalar is the **input-power ratio**:

    ratio = (avg_input_current * INPUT_SUPPLY_VOLTAGE)
            / MAX_INPUT_POWER_SETPOINT_WATTS

i.e. the averaged 48 V-rail current draw normalised against the 75 W
thermal/SOA setpoint. It is a proxy for sparking intensity:

| ratio | interpretation                              |
|-------|---------------------------------------------|
| → 0   | little or no current — wire too far from work, no sparking |
| → 1   | drawing at the SOA limit — gap shorted or arcing |

This is *correlated with* but not identical to discharge success rate
(the fraction of attempted pulses that successfully fire). Success
rate is computed inside the firmware as
`ctx->avg_discharge_success_rate` (`src/output.c` per-tick body) but
is not currently transmitted. Swapping the source is a one-line
change in `periph_tick()` if a more direct gap-condition measurement
is wanted later.

## Source measurement (firmware-side)

ADC sampling lives in `src/sensors.c`:

- `sensors_sample_input_current()` (`sensors.c:71-81`) takes one PMM
  current-sense ADC sample, converts to amps, pushes it into a
  100-deep ring buffer.
- `sensors_avg_input_current_amps()` (`sensors.c:83-92`) returns the
  buffer mean.

At the 100 Hz sample rate (next section), the buffer represents a
~1-second moving average once full. That averaging window is the
dominant smoothing term in the whole feedback path.

## Update cadence

`periph_tick()` in `main.c:129-150` is the only writer of the
feedback PWM. It is invoked from the main loop's gated 100 Hz cadence
(`PERIPH_MGMT_INTERVAL_MS = 10` ms). Each tick:

1. `sensors_sample_input_current()` — one new sample into the ring.
2. `MAX_SAFE_INPUT_CURRENT` envelope check.
3. State decision from `EDM_ENABLE_PIN`.
4. `output_feedback_set_ratio(ratio)` if OPERATING, otherwise
   `output_feedback_disable()`. This is where the PWM frequency is
   reprogrammed (`pwm_set_wrap` + `pwm_set_chan_level`).

So: **the feedback PWM frequency is reprogrammed every 10 ms** while
OPERATING.

## Encoding

Constants live in `config.h`:

    EDM_FEEDBACK_FREQ_MIN_HZ        = 200    # ratio = 0 -> 200 Hz
    EDM_FEEDBACK_FREQ_MAX_HZ        = 400    # ratio = 1 -> 400 Hz
    EDM_FEEDBACK_PWM_DUTY           = 0.5
    EDM_FEEDBACK_PWM_CLOCK_DIVIDER  = 64

Mapping is linear and non-inverted; computed inside
`output_feedback_set_ratio()` (`src/output.c`):

    freq_hz = MIN + ratio * (MAX - MIN)
    wrap    = PWM_BASE_CLOCK_FREQ / (freq_hz * CLOCK_DIVIDER)
    level   = wrap * DUTY

The 200–400 Hz range is chosen for responsiveness rather than
resolution. At 200 Hz the period is 5 ms — half the 10 ms firmware
update interval — so the host sees at least two full cycles between
updates. A wider range (e.g. 50–500 Hz) gives more dynamic range,
but at the low end the cycle period would exceed the update
interval, smearing each new value across multiple host servo-thread
reads before the next update lands.

Practical resolution on the host side is roughly 200 Hz of range
quantised to integer Hz, so ~200 distinct steps over the ratio range.
Adequate for feedrate-scaling; not adequate for tight precision
control.

## Edge cases

- **IDLE / fault / boot**: `output_feedback_disable()` halts the PWM
  and the line idles low (no edges). The host should treat "no
  signal" distinctly from "ratio = 0" — either gate adaptive feed on
  the `EDM_ENABLE` line or rely on HostMot2's `vel-timeout`.
- **EDGE_DETECTION_MODE signaling**: when an edge is detected the
  feedback PWM is driven to `EDM_FEEDBACK_FREQ_MAX_HZ` for 1 s, then
  disabled for 1 s. The motion controller can detect this as a tone
  burst followed by silence — distinguishable from any normal
  in-range ratio.

## LinuxCNC HAL prototype (Mesa 7i76e)

The 7i76e daughtercard's encoder counter, configured for
single-channel edge counting, exposes raw Hz on its `velocity` pin.
Wire `EDM_FEEDBACK_PIN` to the A input of one encoder channel.

Component names and indices in this snippet are illustrative — match
to your actual bitfile and board enumeration.

```hal
# --- 1. Read frequency off the encoder counter -----------------------
setp hm2_5i25.0.encoder.00.counter-mode  1     # single-channel edge count
setp hm2_5i25.0.encoder.00.scale         1     # velocity in raw Hz
setp hm2_5i25.0.encoder.00.vel-timeout   0.05  # zero out below ~20 Hz

net edm-fb-hz <= hm2_5i25.0.encoder.00.velocity

# --- 2. Map Hz back to ratio in [0,1] --------------------------------
# ratio = (hz - FMIN) / (FMAX - FMIN)
# With FMIN=200, FMAX=400: gain = 1/200 = 0.005, offset = -200/200 = -1.0
loadrt scale  names=fb_to_ratio
loadrt limit2 names=fb_clamp
addf fb_to_ratio servo-thread
addf fb_clamp    servo-thread

setp fb_to_ratio.gain    0.005
setp fb_to_ratio.offset -1.0
setp fb_clamp.min        0.0
setp fb_clamp.max        1.0

net edm-fb-hz     => fb_to_ratio.in
net edm-ratio-raw <= fb_to_ratio.out
net edm-ratio-raw => fb_clamp.in
net edm-ratio     <= fb_clamp.out      # <-- 0..1 control variable

# --- 3. Drive feedrate from the ratio --------------------------------
# Option A: dumb linear map. ratio 0 -> full feed, ratio 1 -> 20% feed.
loadrt scale names=ratio_to_feed
addf ratio_to_feed servo-thread
setp ratio_to_feed.gain   -0.8
setp ratio_to_feed.offset  1.0
net edm-ratio     => ratio_to_feed.in
net adaptive-feed <= ratio_to_feed.out
net adaptive-feed => motion.adaptive-feed
setp motion.adaptive-feed-enable 1

# Option B (recommended once tuning starts): PID toward a setpoint
# (e.g. ratio = 0.5 as the sparking sweet spot).
# loadrt pid names=edm_pid
# addf  edm_pid.do-pid-calcs servo-thread
# setp  edm_pid.Pgain      1.0
# setp  edm_pid.Igain      0.2
# setp  edm_pid.maxoutput  0.5    # never boost feed by >50%
# setp  edm_pid.minoutput -0.8    # allow large slowdowns
# net edm-setpoint => edm_pid.command       # constant 0.5 from a setp
# net edm-ratio    => edm_pid.feedback
# net feed-trim    <= edm_pid.output
# ... sum2 with 1.0 nominal, into motion.adaptive-feed
```

### Notes for the host integrator

- **Quantization at the band edges.** With `vel-timeout = 50 ms`, the
  HostMot2 estimator sees ~10 cycles at 200 Hz and ~20 at 400 Hz per
  window — enough for a stable velocity estimate. Shortening the
  timeout reduces latency but raises noise; lengthening it does the
  opposite. A `lowpass` on `edm-ratio` is a good knob to expose.
- **IDLE handling.** When firmware calls `output_feedback_disable()`
  the line is silent and `vel-timeout` will zero the velocity, which
  the linear-map Option A interprets as "full feed." Either gate
  `motion.adaptive-feed-enable` off the `EDM_ENABLE` electrical
  counterpart or use a `mux2` to substitute a safe value when the
  enable line is low.
- **Edge-mode signaling.** The 1 s `MAX_HZ` tone followed by 1 s of
  silence at the start of edge detection will be visible on
  `edm-ratio` as a brief ramp to 1.0 then 0.0. If the motion
  controller is going to act on this it needs its own debounce — a
  spurious ratio = 1 sample inside the tone burst would trigger
  Option A's slowdown.

## Latency budget

End-to-end response from "input current changes" to "motion
controller acts":

| stage                                | typical          |
|--------------------------------------|------------------|
| ADC sample → ring buffer push        | < 1 ms           |
| 1 s moving-average smoothing         | up to 1 s (dominant) |
| Firmware tick → PWM reprogram        | up to 10 ms      |
| 1–2 PWM cycles on the wire           | 2.5–10 ms        |
| HostMot2 velocity estimate           | ~1 servo period (1 ms) |
| HAL chain (scale + clamp + feedrate) | ~1 ms per addf   |

The 1 s ring-buffer average is by far the dominant smoothing term;
the wire-and-host transport adds ~15–25 ms of worst-case delay on top
of that. Tighten the average via `DEVICE_CURRENT_BUFFER_SIZE` if the
control loop needs faster response.
