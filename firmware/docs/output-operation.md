# Powercore V3 — Output Stage Operation

## Output Switches

Three MOSFETs form the output stage, all driven by hardware PWM from the RP2040:

| Pin | Signal | Type | Logic | Function |
|-----|--------|------|-------|----------|
| GP8 | `SW_HIGH_VOLTAGE_PHASE` | P-channel | Inverted (HIGH=OFF, LOW=ON) | Connects boost converter (64–100 V) to (+) output to pre-charge output capacitor to spark initiation voltage |
| GP10 | `SW_ENABLE` | N-channel | Normal (HIGH=ON, LOW=OFF) | Connects (–) electrode to GND through output current sensor (TMCS1133) |
| GP11 | `SW_HIGH_CURRENT_PHASE` | P-channel | Inverted (HIGH=OFF, LOW=ON) | Connects 48 V high-current phase (pi-filter output) to (+) output to drive machining current |

In the safe/disabled state: `SW_ENABLE` is driven LOW, both P-channel gates are driven HIGH via GPIO (overriding PWM).

---

## Timing — Hardware PWM, No Interrupts

All switch timing is handled entirely by the RP2040 PWM hardware. There are no ISRs, delays, or software loops involved in the switching cadence. Once `pwm_set_enabled()` is called, the hardware runs autonomously.

**Phase-correct (center-aligned) mode** is used on all slices. The counter counts up to `wrap`, then back down to 0. The output is HIGH when `counter ≤ level` on both the up- and down-count, producing a symmetric pulse centered at counter = 0.

```
pwmWrapValue  = 133,000,000 / (frequency_Hz × 2)
pwmLevelValue = pwmWrapValue × dutyCycle
```

At 10 kHz, 10% duty cycle: `wrap = 6,650`, `level = 665`, period = 100 μs, on-time = 10 μs.

---

## Two PWM Slices

The three switches are split across two slices, both configured identically and enabled simultaneously:

| Slice | Pins | Switches | Level |
|-------|------|----------|-------|
| Slice 5 | GP10, GP11 | `SW_ENABLE` + `SW_HIGH_CURRENT_PHASE` | `pwmLevelValue` (machining duty cycle) |
| Slice 4 | GP8, GP9 | `SW_HIGH_VOLTAGE_PHASE` + `OUTPUT_OVERCURRENT_SET` | `pwmHighVoltageLevelValue` (fixed 1 μs) |

P-channel polarity inversion is handled in hardware via `pwm_set_output_polarity()`, so both switches on Slice 5 turn on and off together with no additional logic.

---

## Switching Sequence Per EDM Cycle

The HV slice counter is pre-loaded to 25% of `wrap` before being enabled:

```
pwm_set_counter(slice4, wrap × 0.25)  // = 1,662 counts at 10 kHz
```

Both slices then run at identical rates, so this offset is fixed. The result at 10 kHz:

```
t = 0–10 μs    Machining pulse — SW_ENABLE + SW_HIGH_CURRENT_PHASE ON
               48 V applied across gap; 40–70 A flows through discharge
t = 10–87 μs   All switches OFF — gap de-ionizes, dielectric recovers
t = 87–88 μs   HV pre-charge pulse — SW_HIGH_VOLTAGE_PHASE ON (~1 μs)
               Boost converter charges output capacitor to spark initiation voltage (64–100 V)
t = 88–100 μs  All switches OFF — ~7 μs settling before next machining pulse
```

The 7 μs gap between the HV pulse and the machining pulse prevents shoot-through (both phases conducting simultaneously). The 1 μs HV pulse duration and 25% offset are fixed constants in the firmware (`EDMIsofrequencyModeHighVoltagePulseOnTimeMicros = 1`, `EDMIsofrequencyModeHighVoltagePwmOffsetValue = 0.25`).

---

## Discharge Detection

The TMCS1133 output current sensor has a hardware comparator output (`OUTPUT_OVERCURRENT`, GP12) that asserts on a falling edge when machining current exceeds the threshold (default 8 A). This fires an ISR that:

1. Snapshots the output current and voltage ADC readings
2. Increments the discharge counters
3. Sets the `newDischargeDetected` flag for the main loop

The ISR does **not** modify switch state. The main loop processes the flag on its next iteration.

The overcurrent threshold level is set via PWM on `OUTPUT_OVERCURRENT_SET` (GP9, same slice as `SW_HIGH_VOLTAGE_PHASE`), calculated as:

```
threshold_voltage = (0.025 V/A × threshold_A) / 2.5
threshold_level   = (threshold_voltage / 3.3) × wrap
```

---

## Pulse Skipping

The main loop (`EDMIsofrequencyMode()`) monitors two conditions and suppresses output by calling `disableOutputStage()` when either is exceeded:

| Condition | Threshold | Pause Duration |
|-----------|-----------|----------------|
| Discharge success rate | > 80% over calculation window | 250 ms |
| Average input power | > 75 W | 10 ms |

After a pause, `setupOutputPWMForEDMIsofrequencyMode()` re-enables the PWM. Power consumption is reported to the external motion controller via the `EDM_FEEDBACK` PWM pin (GP3, 1 kHz, active-low: 0% = at power limit, 100% = no load).

---

