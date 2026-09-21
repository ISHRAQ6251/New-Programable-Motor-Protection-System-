# Power calculation, display, and logging — Design

Date: 2026-09-14
Status: Approved (uses only data the firmware already acquires; no new sensors or sampling passes).
Scope: Extend v2 firmware in `MotorProtection/`. Not a rewrite. Not compiled in this environment.

## 1. Purpose

Compute, show on the dashboard, and log the electrical power of every configured motor from the samples the protection task already takes every pass. DC yield is true power in watts; AC yield is apparent power in volt-amperes, because AC voltage is a static nameplate field, not a measurement.

## 2. What the hardware can actually measure

| Motor type | Current | Voltage | Reported quantity |
|---|---|---|---|
| DC | measured (20 ms mean of abs, per channel) | measured per channel (ADS1115, downstream of relay) | true power `W` |
| AC 1-phase | measured (true RMS) | not measured, rated V from NVS | apparent power `VA` at rated V |
| AC 3-phase | measured (true RMS, 3 channels) | not measured, rated V from NVS | apparent power `VA`, balanced-voltage assumption |

Power factor cannot be measured with this hardware. AC values are always `VA`, never `W`. AC voltage sensing (ZMPT101B) stays descoped.

## 3. Locked decisions

| Topic | Decision |
|---|---|
| Where computed | `protection.cpp`, in the same locked region that applies I²t, from the off-mutex samples of that pass |
| Extra sampling | None. Reuse `SampleResult` / `VoltageSample` already taken |
| Sample rate | One value per protection pass (~1 Hz), consistent with dashboard poll |
| Units | DC → `W`; AC → `VA`. Unit also fixes the energy label (`Wh` / `VAh`) |
| Storage | RAM only, in `MotorRuntime`. No `MotorRecord` change, no NVS schema bump |
| Lifetime | Accumulates only while Running. Displays `0.0` in Stopped / Fault / Cooling. Resets on boot |
| Logging | Fault CSV only, on trip. No periodic SD writes |

## 4. Math (per protection pass, while Running)

```
DC:
    P_W  = V_ch * I_dc_ch
    E   += P_W  * dt_s / 3600      // Wh

AC 1-phase:
    S_VA = V_rated * I_rms_ch
    E   += S_VA * dt_s / 3600      // VAh

AC 3-phase (rated V is line-to-line):
    S_VA = (V_rated / sqrt(3)) * (I_a + I_b + I_c)
    E   += S_VA * dt_s / 3600      // VAh
```

The 3-phase sum-of-currents form equals `sqrt(3) * V_LL * I_avg` when currents are balanced, and degrades sensibly under current unbalance (voltage is still assumed balanced). Energy never decays while Running; it is zeroed when the motor leaves Running.

## 5. Data model changes

`MotorRuntime` gains:

```c
float   power;       // latest W (DC) or VA (AC); 0 when not Running
float   energy;      // Wh (DC) or VAh (AC) since boot / last start
uint8_t power_is_w;  // 1 -> W, 0 -> VA
```

`LogEvent` gains:

```c
float   power;       // W if power_is_w, else VA
uint8_t power_is_w;
```

`MotorRecord` and `MotorBlob` were unchanged by this power delta, so `NVS_SCHEMA` stayed 2 at the time. A later stall-recovery field bumped the live schema to 3.

## 6. Behavior

- `computePower()` runs right after `applySample()` for each Running motor and before any trip is raised, so the tripping window's power is available to the log.
- `trip()` snapshots `power` / `power_is_w` before `zeroEnergy()`, so the log carries the window that tripped even though the runtime then resets.
- `zeroEnergy()` also clears `power` and `energy`; the idle branch of the task clears them too, keeping Stopped / Fault / Cooling at `0.0`.
- DC during the 250 ms UV/OV grace after Start: power is computed and accumulated normally (V and I are both real there).
- `SENSOR_FAULT` logs the power computed from the same reading the `current_A` column already reflects.
- DC sums per channel (`Σ V_ch × I_ch`). A single-phase DC motor has one term; the unusual 3-phase DC record sums all three, matching the per-channel voltage taps and UV/OV checks.

## 7. Rounding

| Quantity | Format |
|---|---|
| `W` | 1 decimal (`142.7 W`) |
| `VA` | integer, prefixed `~ ` in the UI (`~ 450 VA`) |
| Energy | 2 decimals (`1.23 Wh` / `1.23 VAh`) |

## 8. Fault CSV

New header:

```
uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA
```

DC rows fill `power_W`; AC rows fill `power_VA`; the other column stays blank. Writers always emit 7 fields; a blank field is an empty string between commas. Older 4- or 5-column rows still parse (missing fields read as blank).

## 9. API

`GET /api/status` per-motor object gains:

```json
"power": 142.7,
"power_unit": "W",
"energy": 1.23
```

`GET /api/log` rows gain `"power_W"` and `"power_VA"` strings (blank when absent).

## 10. UI

- Dashboard table gains a **Power** column. DC shows `142.7 W`; AC shows `~ 450 VA`. A sub-line shows `1.23 Wh since boot` (or `VAh`).
- Log table gains **Power (W)** and **Power (VA)** columns; whichever does not apply is blank.

## 11. Documentation

- `docs/USER_MANUAL.md`: new "Power display and its limits" section.
- `AGENTS.md`: module row, assumptions, bench notes.
- `README.md`: one-line feature mention.

## 12. Bench acceptance

1. DC known load: displayed W within sensor tolerance of a bench meter; `P → 0` when the relay opens.
2. AC 1-phase: hand-check `S = V_rated * I_rms`.
3. AC 3-phase: unbalance two phases; confirm `S` uses the sum of currents, not the maximum.
4. Trip: fault CSV line carries power in the correct column.
5. Reboot: energy reads `0.00` and the label says "since boot".

## 13. Out of scope / do not

- Re-open AC voltage sensing / ZMPT101B.
- Present `VA` as `W` or imply a power factor.
- Write energy to NVS per pass, or bump the motors blob schema.
- Add sampling passes, or hold the status mutex across `analogRead` / I2C.
- Persistence, export beyond the fault CSV, per-pass NVS writes.
