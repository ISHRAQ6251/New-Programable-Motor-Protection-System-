# DC voltage sensing + AC rated-voltage field — Design

Date: 2026-09-13 (updated to live firmware as of `700db56`)
Status: Approved (hardware, Adafruit driver, UV/OV optional, tap downstream of relay). Live UV/OV grace is 750 ms; ADS data rate is 128 SPS; NVS schema is 4.
Scope: Extend v1 firmware in `MotorProtection/`. Not a rewrite. Not compiled in this environment.

## 1. Purpose

Add live DC bus voltage sensing (8 channels via 2x ADS1115) with optional undervoltage/overvoltage trip, and a static rated-AC-voltage configuration field (no AC sensing hardware).

## 2. Locked decisions

| Topic | Decision |
|---|---|
| Current sensing | Unchanged. ESP32 ADC1 GPIO 1,2,4,5,6,7,8,9. `sensing.cpp` stays current-only |
| Voltage ADC | 2x ADS1115, I2C, not ESP32 ADC |
| Driver | Adafruit ADS1X15 (+ Adafruit BusIO). No custom register driver |
| I2C pins | GPIO 14 SDA, GPIO 42 SCL. `Wire.begin(14, 42)` never default 8/9 |
| ADS addresses | 0x48 ADDR=GND → CH4–7 AIN0–3; 0x49 ADDR=VDD → CH0–3 AIN0–3 |
| Gain | `GAIN_ONE` (±4.096 V) set explicitly after `begin()`, not library default |
| Divider | R1=150 kΩ, R2=10 kΩ, scale 16. 0–50 V → ~0–3.13 V |
| Tap point | Each motor's terminal voltage **downstream of its relay**, not the shared bus |
| Zero cal | Same idle-cal path as current. Stopped = relay open = ~0 V at tap |
| UV/OV | DC only. 0 disables that trip independently. If both > 0, OV must be > UV |
| AC voltage | Manual "Rated AC voltage" per motor. NVS only. Not live. Not in trip logic |
| ZMPT101B | Descoping closed. Do not re-open |
| Sampling | Voltage reads **off-mutex**, same pattern as current |
| Start grace | UV/OV ignored for 750 ms after entering Running (relay just closed) |
| Module split | New `voltage.cpp` / `voltage.h` |

## 3. I2C pin re-bind (Adafruit BusIO)

`Adafruit_I2CDevice::begin()` calls `Wire.begin()` with no arguments. On ESP32-S3 that reverts to GPIO 8/9, which are current-sense channels.

Firmware must:

1. `Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL)` before each `ads.begin()`
2. Call it again immediately after each `ads.begin()`
3. Probe 0x48 / 0x49 with `Wire.endTransmission()` on the rebound bus
4. `setGain(GAIN_ONE)` and `setDataRate(RATE_ADS1115_128SPS)` after begin

Presence = probe ACK after re-bind, not the boolean from `ads.begin()` alone.

## 4. Conversion

```
V_adc  = ads.computeVolts(raw)          // volts at AIN, GAIN_ONE
V_bus  = (V_adc - v_zero) * 16.0        //  (R1+R2)/R2
```

`v_zero` is the mean AIN voltage with the relay open (calibrate). Never assume 0.000 V.

Sensor-fault (voltage path, Running DC only, skipped while jam-release pulses run):

- ADS missing / I2C fail on that channel's chip (`present=0`)
- `|V_adc|` near full-scale (> 4.0 V) or `|V_bus|` > 55 V

Voltage-path `SENSOR_FAULT` trips only after 3 consecutive faults within 1 s. Current-path `SENSOR_FAULT` (stuck ADC, Vadc outside `[0.05, 3.05]` V, |I| > 40 A) is immediate and is checked before stall.

UV: filtered `V_bus < uv_volts` when `uv_volts > 0`, after 750 ms grace.
OV: filtered `V_bus > ov_volts` when `ov_volts > 0`, after 750 ms grace.
Each ADS reading is the average of 4 conversions, then a 0.9/0.1 LPF on `Vbus`.

Trip order in one window: current `SENSOR_FAULT`, `STALL`, voltage `SENSOR_FAULT` (3 consecutive faults in 1 s), `OV`, `UV`, `I2T`. Same Cooling / auto-restart / Reset as v1.

DC Start is rejected if the ADS1115 for that motor's voltage sense channel is missing (dedicated `vch`, or every current channel when `VCH_SAME`). AC motors run without the ADS chips.

## 5. Data model

Live firmware uses `NVS_SCHEMA = 4` (`voltage_channel` plus later inrush fields). Size/schema mismatch still starts empty. Older v1/v2/v3 blobs are discarded.

`MotorRecord` adds:

- `rated_ac_v` — AC only, required > 0 when `is_ac`. Stored 0 for DC
- `uv_volts`, `ov_volts` — DC only, 0 = disabled. Stored 0 for AC
- `voltage_channel` — DC: 0–7 or `VCH_SAME` (0xFF = same ADS tap as current CH0). AC forces `VCH_SAME`

`ChannelRuntime` adds `v_zero`, `last_v`, `v_calibrated`.

`FaultType` adds `FT_UNDERVOLT`, `FT_OVERVOLT`.

Fault CSV: `uptime_ms,motor,type,current_A,voltage_V`. Types include `UNDERVOLT`, `OVERVOLT`. (Superseded by the 2026-09-14 power spec, which appends `power_W,power_VA`.)

## 6. UI

- Dashboard: live voltage column, same tier as RMS and thermal %. DC shows live V; AC shows an em dash (not rated V — rated V is config, not telemetry)
- Add/Edit: AC shows rated AC voltage + Hz, hides UV/OV. DC shows UV/OV (placeholder 0 = off), hides Hz and rated AC
- Calibrate button zeros current and DC voltage together (all motors Stopped/Fault)
- Meta line includes ADS ok/missing

Apparent power (`V × I`) was out of scope here; it is now implemented — see `2026-09-14-power-display-design.md`.

## 7. Files

| File | Change |
|---|---|
| `config_pins.h` | I2C pins, ADS addresses, divider constants |
| `config_limits.h` | Schema 4, voltage cal samples, Vbus cap, UV grace 750 ms |
| `types.h` | Record fields, fault types, snapshot volts / ads_ok |
| `voltage.h/.cpp` | New. ADS1115, cal, sample |
| `sensing.cpp` | Unchanged role (current only) |
| `protection.cpp` | Off-mutex voltage sample, UV/OV/SENSOR, snapshot |
| `motor_store.cpp` | Validate new fields |
| `web.cpp` / `web_html.h` | Form fields, live V, log column, polish |
| `sd_log.cpp` | New CSV columns / type names |
| `MotorProtection.ino` | `voltageBegin()`, library comment |
| `AGENTS.md`, `README.md`, `docs/USER_MANUAL.md` | Hardware, fields, ZMPT descoped |
