# ESP32-S3 Programmable Motor Protection Firmware — Design

Date: 2026-09-02 (updated 2026-09-13 for the DC-voltage / rated-AC extension, 2026-09-14 for the power display / logging extension, 2026-09-21 for stall recovery / RAM fault ring)
Status: Approved. v1 + DC voltage / AC rated-V field + power display + stall recovery (jam release) + 32-entry RAM fault ring implemented in `MotorProtection/` (not compiled in this environment; committed). Focused delta specs: `docs/superpowers/specs/2026-09-13-dc-voltage-sensing-design.md` and `docs/superpowers/specs/2026-09-14-power-display-design.md`.
Scope: ESP32-S3 firmware only. Sensors, relays, SD breakout, voltage taps, and buzzer are treated as already wired.

## 1. Purpose

Replace a bimetallic thermal overload relay with a microcontroller I²t protector. The firmware measures motor current, accumulates I²t energy, and trips relay(s) before the motor overheats. It also provides live monitoring, configurable multi-step trip curves, fault logging, and a desktop web dashboard on the ESP32 SoftAP.

Correctness and code clarity matter more than polish. This is a university engineering project.

## 2. Locked decisions

| Topic | Decision |
|---|---|
| Toolchain | Arduino IDE sketch (folder + `.ino` + `.h`/`.cpp` tabs) |
| Board | ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Architecture | Dual-task: high-priority protection task + `loop()` service path |
| Wi-Fi | SoftAP only. No STA, no WiFiManager, no DDNS, no captive portal |
| AP credentials | SSID `MPS-505`, password `mps50005`, IP `192.168.4.1` |
| Web auth | Themed `/login` page + HttpOnly session cookie, default `mps` / `mps500`, stored in NVS |
| ACS712 | ACS712-30A, 66 mV/A, all channels |
| Current divider | R1=10 kΩ, R2=15 kΩ, ratio 0.6 |
| DC voltage | 2× ADS1115 on I²C (0x48 / 0x49), `GAIN_ONE`, tap downstream of each relay |
| Voltage divider | R1=150 kΩ, R2=10 kΩ, scale 16. Zero calibrated with relay open |
| UV / OV trip | DC only, optional: `0` disables each independently; if both set, OV > UV |
| Rated AC voltage | AC nameplate only, stored in NVS. Never sensed, never a trip input |
| I²C pins | GPIO 14 SDA / 42 SCL. Explicit `Wire.begin(14, 42)`, not default 8/9 |
| Relay default | Active-LOW, OFF=HIGH. De-energized on boot. Per-channel override in motor record |
| Mains frequency | Per motor, 50 or 60 Hz (AC only) |
| I²t model | Classic energy: `E += I_rms² × dt`, trip vs `(k×In)² × t_trip` |
| Below pickup | Decay `E` toward 0 over that motor's cooling time |
| Stall | Immediate on the next RMS window, independent of I²t |
| Sensor fault | Trip immediately (`SENSOR_FAULT`). Auto-restart still applies |
| Protection steps | Max 8 per motor |
| Max motors / channels | 8 / 8 |
| SD | Fault log only. Missing SD does not affect protection or dashboard |
| Timebase | `millis()` since boot. No NTP (SoftAP has no upstream time) |

## 3. Architecture

Arduino `setup()`/`loop()` plus FreeRTOS.

```
                  +------------------+
  ACS712 ADC  --> | Protection task  | --> Relays
                  | (core 1, prio 5) |
                  | RMS, I2t, stall, |
                  | sensor fault     |
                  +--------+---------+
                           | command queue (fixed struct)
                           | status snapshot (mutex)
                  +--------v---------+
  Browser <-----> | loop() + Async   | --> Buzzer (LEDC)
  SoftAP          | web server       | --> SD log writes
                  | (core 1, prio 1) |
                  +------------------+
         WiFi/tcpip on core 0
```

Rules:

- The protection task never mounts SD, never formats strings for HTTP, and never blocks on Wi-Fi.
- Relays are driven only from the protection task (and from `setup()` fail-safe OFF before tasks start).
- Web handlers enqueue commands (`START`, `STOP`, `RESET`, `CALIBRATE`, motor CRUD already applied to NVS then `RELOAD`).
- Live dashboard reads a mutex-guarded snapshot (`rms[8]`, `volts[8]`, `energy`, `power`, `status`, `uptime`, `fault_count`, `thermal_pct`, `ads_ok[2]`, `v_calibrated[8]`, `last_fault`, `sd_ok`, `free_heap`).
- Current ADC and ADS1115 are sampled **outside** the status mutex, then the mutex is re-taken to apply I²t / UV / OV / stall / sensor-fault. The mutex is never held across `analogRead` or I²C.

### 3.1 File split

Arduino IDE sketch folder `MotorProtection/`:

| File | Responsibility |
|---|---|
| `MotorProtection.ino` | `setup()`/`loop()`, task spawn, boot Serial banner |
| `config_pins.h` | Only file with GPIO numbers, mV/A, I²C pins/addresses, voltage-divider constants |
| `config_limits.h` | `MAX_CHANNELS`, `MAX_MOTORS`, `MAX_STEPS`, string lengths, `NVS_SCHEMA` |
| `types.h` | Shared enums/structs (motor record, commands, status, volts/ads_ok) |
| `motor_store.h/.cpp` | NVS load/save of motor list + auth credentials; field validation |
| `sensing.h/.cpp` | Current ADC, zero calibration, RMS / DC average |
| `voltage.h/.cpp` | 2× ADS1115 DC voltage: re-bind Wire, calibrate zeros, sample |
| `protection.h/.cpp` | I²t, stall, UV/OV, cooling, motor state machine |
| `relays.h/.cpp` | Polarity-aware coil drive, boot fail-safe OFF |
| `buzzer.h/.cpp` | Named non-blocking LEDC tunes |
| `sd_log.h/.cpp` | Optional CSV log; safe no-op if unmounted |
| `net_ap.h/.cpp` | SoftAP start, boot network print |
| `web.h/.cpp` | ESPAsyncWebServer, session login, HTML/JSON |

No GPIO literals outside `config_pins.h`.

## 4. Hardware and pin map

Module: ESP32-S3-N16R8. GPIO 22–25 do not exist on this chip at all (not physical pins). GPIO 26–37 are reserved for flash/octal PSRAM on this N16R8 module. GPIO 0/45 remain fully off-limits (boot mode / flash voltage). GPIO 3 is ENC_SW (strapping-safe with the KY-040 pull-up). GPIO 46 is input-only/strapping and is ENC_B (DT). GPIO 19/20 are native USB D-/D+ and must never be used as GPIO (USB CDC is the serial path). GPIO 48 is the onboard WS2812. GPIO 43/44 stay untouched.

| Function | GPIO | Notes |
|---|---|---|
| I-sense CH0 | 1 | ADC1 |
| I-sense CH1 | 2 | ADC1 |
| I-sense CH2 | 4 | ADC1 |
| I-sense CH3 | 5 | ADC1 |
| I-sense CH4 | 6 | ADC1 |
| I-sense CH5 | 7 | ADC1 |
| I-sense CH6 | 8 | ADC1 |
| I-sense CH7 | 9 | ADC1 |
| Relay CH0 | 15 | Active-LOW default |
| Relay CH1 | 16 | |
| Relay CH2 | 17 | |
| Relay CH3 | 18 | |
| Relay CH4 | 38 | |
| Relay CH5 | 39 | |
| Relay CH6 | 40 | |
| Relay CH7 | 41 | |
| SD MOSI | 11 | SPI, 3.3 V only |
| SD MISO | 13 | |
| SD SCK | 12 | |
| SD CS | 10 | |
| Buzzer | 21 | LEDC PWM, passive |
| I²C SDA | 14 | Shared ADS1115 + SH1106 — explicit `Wire.begin(14, 42)`, not default 8/9 |
| I²C SCL | 42 | Shared ADS1115 + SH1106 |
| Encoder CLK | 47 | KY-040 A |
| Encoder DT | 46 | KY-040 B; GPIO 46 input-only, encoder line only |
| Encoder SW | 3 | KY-040 switch; strapping-safe with module pull-up. GPIO 19 is USB D- and must never be used as GPIO |
| Status LED | 48 | Onboard WS2812, one-wire — never take `s_i2c_mu` |

Analog path (current):

- ACS712-30A on 5 V, Vout = 2.5 V + 0.066 V/A × I
- Divider 0.6 → Vadc ≈ 1.5 V + 0.0396 V/A × I
- ESP32 ADC 12-bit, 11 dB attenuation (full-scale near 3.1–3.3 V)
- Sensitivity used in firmware: `MV_PER_AMP = 66.0f * 0.6f` (39.6 mV/A) relative to the calibrated zero, not relative to theoretical 1.5 V
- Zero-current ADC counts are measured at runtime per channel

Analog path (DC voltage):

- 2× ADS1115 on I²C: 0x48 (ADDR→GND) = CH4–7 AIN0–3; 0x49 (ADDR→VDD) = CH0–3 AIN0–3
- Per channel divider R1=150 kΩ / R2=10 kΩ (scale 16), 0–50 V → ~0–3.13 V
- Gain `GAIN_ONE` (±4.096 V), data rate 250 SPS, set explicitly after `begin()`
- `V_bus = (V_adc - v_zero) * 16.0`; `v_zero` is calibrated with the relay open, never assumed 0 V
- Tap is the motor terminal **downstream of that channel's relay**, not the shared bus
- Adafruit BusIO's `ads.begin()` calls `Wire.begin()` with no pins; firmware re-binds GPIO 14/42 before and after each `begin()`

Relays:

- `setup()` sets every relay pin OUTPUT HIGH before Wi-Fi or tasks start (active-LOW default, OFF=HIGH)
- Energize = GPIO LOW when polarity is active-LOW; inverted when the motor record says active-HIGH
- MCU reset / boot: pins driven HIGH → coils de-energized for the default polarity

## 5. Data model

### 5.1 Motor record (NVS, not SD)

Fixed-size struct, packed, versioned (`NVS_SCHEMA = 3`). A size/schema mismatch (e.g. an old v1/v2 blob) starts empty.

```
MotorRecord
  uint8_t  used
  char     name[24]
  uint8_t  phase_count          // 1 or 3
  uint8_t  channels[3]          // channel indices 0..7; unused = 0xFF
  uint8_t  is_ac                // 1 = AC, 0 = DC
  uint8_t  mains_hz             // 50 or 60; ignored if DC
  float    rated_ac_v           // AC nameplate only; 0 for DC; not a trip input
  float    in_amps              // operating current
  float    stall_amps
  float    cooling_s
  uint8_t  auto_restart         // 0/1
  uint8_t  stall_recovery       // 0/1 jam release; default 0
  uint8_t  relay_active_high[3] // 0 = active-LOW (default)
  uint8_t  step_count           // 1..8
  float    step_k[8]            // multiplier of In
  float    step_t_s[8]          // trip time at that multiple
  float    uv_volts             // DC undervoltage; 0 = disabled; 0 for AC
  float    ov_volts             // DC overvoltage; 0 = disabled; 0 for AC
```

Validation: AC requires `mains_hz` 50/60 and `rated_ac_v` in 0.1–1000. DC requires `uv_volts`, `ov_volts` ≥ 0 and ≤ 55, and `ov_volts > uv_volts` when both > 0. Cross-field values are zeroed on add/edit (AC stores no UV/OV; DC stores no rated AC).

NVS namespace `mps`. Keys:

- `motors` — blob of `MotorRecord[MAX_MOTORS]`
- `auth_user`, `auth_pass` — dashboard login credentials
- schema marker

Corrupt, missing, or wrong-schema blob → empty motor list, Serial warning, do not invent motors. Protection and web still run.

### 5.2 Runtime (RAM only)

Per channel: `zero_adc`, `last_rms`, `energy_a2s` (I²t accumulator), `v_zero`, `last_v`, `v_calibrated`, last sample ticks.

Per motor: `status`, `uptime_ms`, `fault_count`, `cooling_deadline_ms`, last fault type, `power` (latest `W` or `VA`), `energy` (`Wh` / `VAh`, RAM only), jam-release `jam_count` / `jam_phase` / `jam_deadline_ms`. Power/energy update once per protection pass, show `0.0` when not Running, and are never written to NVS. See `2026-09-14-power-display-design.md`.

Dashboard thermal % for a motor is the max of its channels' `100 * E / E_trip`. For 3-phase that is the hottest phase.

Statuses: `Stopped`, `Running`, `Fault`, `Cooling`.

Dashboard is one row per motor, never per channel. A 3-phase motor is one record occupying three linked channels. Energy is **per channel**; a trip on any assigned channel de-energizes the whole motor.

### 5.3 Channel allocation

Add-motor asks phase count first. Firmware allocates 1 or 3 currently free channels (lowest indices). If not enough free channels, the API returns an error and the UI shows it. Channels are released on delete.

Delete is allowed only from `Stopped` or `Fault`, never from `Running` or `Cooling`.

## 6. I²t and protection logic

### 6.1 Sampling

- AC: at least 32 samples spanning at least one full cycle (20 ms at 50 Hz, 16.67 ms at 60 Hz). True RMS: `sqrt(mean(i²))`. Never a single instantaneous read.
- DC: mean of |i| over a 20 ms window (same function, no RMS).
- 3-phase: all three channels sampled in one pass; any phase may trip the group.
- Motors are scanned round-robin. Worst-case stall detect latency is `N_running × window` (documented; acceptable vs thermal timescales).
- Current: `i = (vadc - vzero) / 0.0396` amperes.

Calibration:

- All 8 channels at boot (motors are still Stopped, relays OFF).
- On-demand from the UI only if every motor is `Stopped` or `Fault`. Otherwise reject.
- Each channel: N samples with no current expected; store mean ADC as `zero_adc`.
- DC voltage zero: same window, with relays OFF the tap sits at ~0 V. Store mean AIN volts as `v_zero` and set `v_calibrated`. Calibration runs ADC and I²C outside the status mutex (channels copied out, zeros copied back).

### 6.2 Energy model

While `Running` and `I_rms >= k_min × In` (lowest step multiplier):

- `E += I_rms² × dt`  (A²s)
- Active step = highest k such that `I_rms >= k × In`
- Trip `I2T` when `E >= (k × In)² × t_trip` of that active step

While `Running` and `I_rms < k_min × In`:

- Decay: `E = max(0, E * (1 - dt / cooling_s))`

`E` is also zeroed on Stop, on Reset, and when leaving Cooling into Stopped.

Thermal load % (dashboard): `0` below pickup; otherwise `100 * E / E_trip` of the active step, clamped 0–100+.

### 6.3 Stall

If `I_rms >= stall_amps` after one sample window → trip `STALL` immediately, unless `stall_recovery` is enabled. No I²t involvement on the stall path.

Optional jam release (fixed constants, not user-timed): after `JAM_RELEASE_MIN_RUN_MS` (500) of Running, de-energize `JAM_RELEASE_OFF_MS` (300), re-energize, wait `JAM_RELEASE_WAIT_MS` (500), re-check. Up to `JAM_RELEASE_MAX` (4) pulses. Success zeros `jam_count` only (not the auto-restart counter). Exhaustion trips `STALL` and Cooling as usual. While `jam_phase != JAM_IDLE`: skip stall / UV / OV / `NO_CURRENT`; keep I²t accumulation and `SENSOR_FAULT`. No extra grace after jam ends.

### 6.4 Sensor fault

Trip `SENSOR_FAULT` immediately if, on an assigned channel:

- ADC reading is stuck (unchanged across a full window within 1 LSB) while the motor is Running (AC or DC), or
- Mean Vadc is outside `[0.05 V, 3.05 V]` (open/shorted divider), or
- Computed |I_rms| is physically implausible (> 40 A on a 30 A sensor)
- DC voltage path (Running DC only): the ADS1115 for that channel is missing / I²C fails, `|V_adc| > 4.0 V`, or `|V_bus| > 55 V`

Same trip path as other faults (relays off, tone, log, cooling). Auto-restart still applies.

Running and every phase `I_rms < 0.05 × In` for 2 s → `NO_CURRENT` (broken sense wire, open winding, or a relay that never closed).

### 6.5 DC undervoltage / overvoltage

DC only, after 250 ms of `Running` (relay just closed): `V_bus < uv_volts` when `uv_volts > 0` → `UNDERVOLT`; `V_bus > ov_volts` when `ov_volts > 0` → `OVERVOLT`. `0` disables that trip independently. Same Cooling / auto-restart / Reset as I²t. AC rated voltage is never compared.

Trip order within one sample window: `SENSOR`, `STALL`, `OV`, `UV`, `I2T`.

### 6.6 State machine

```
Stopped --Start--> Running
Running --Stop---> Stopped
Running --trip---> Cooling     (relays OFF, fault tune, SD log)
Cooling --timer, auto_restart=1 and consecutive trips < 3--> Running
Cooling --timer, auto_restart=0 or 3 consecutive trips--> Fault
Cooling --Reset--> Stopped     (cancel auto-restart, buzzer off)
Fault   --Reset--> Stopped     (buzzer off)
```

Start is rejected from Fault, Cooling, if channels are not calibrated, or (DC) if the ADS1115 for that motor's channels is missing. Auto-restart re-checks the same ADS precondition.

On trip, 3-phase de-energizes all three relays together.

### 6.7 Fail-safe summary

- Boot: all relays OFF before any other init
- Protection must not depend on SD
- NVS failure: no motors, not a crash
- Sensor death: trip, do not keep the coil energized
- Web auth required on every route including JSON

## 7. Connectivity and web UI

SoftAP only: `WiFi.softAP("MPS-505", "mps50005")`, IP `192.168.4.1`. No STA, no WiFiManager, no DDNS. WPA2 password is ≥ 8 characters.

Boot Serial always prints:

- Mode: SoftAP
- SSID, password, IP
- Dashboard user/password (from NVS, default `mps` / `mps500`)

Themed `/login` page (same dark dashboard theme). Successful POST sets an HttpOnly `mps_sess` cookie (8 h). Every other route requires a valid session; unauthenticated HTML redirects to `/login`, JSON returns 401. Credentials changeable on the Security page (session is cleared). `/logout` clears the cookie.

Desktop-only UI (wide table layout, not mobile-first). Four operator pages plus Security, same dashboard for any client on the AP:

1. Dashboard — columns: channels, name, status, uptime, fault count, live RMS (3-phase shows three currents in one cell), live DC voltage (em dash for AC), power (true `W` for DC, apparent `VA` for AC) with session energy, thermal load % (hottest phase), last fault tag, Start/Stop, Reset (enabled in Fault or Cooling). A meta line shows ADS1115 ok/missing. Poll `GET /api/status` about once per second. See `2026-09-14-power-display-design.md`.
2. Add motor — wizard: phase count → allocated channels shown → remaining fields including N then N× (k, t_trip). AC shows mains Hz + rated AC voltage; DC shows optional UV/OV (0 = off). Stall-recovery checkbox (jam release; default off).
3. Edit / delete — same fields as add.
4. Log — SD CSV when mounted; otherwise the 32-entry RAM ring with `ram_only` and a “RAM buffer only — logs lost on reboot” banner. Export CSV is SD-only. Clear empties the RAM ring always, and also rewrites the SD header when a card is mounted.
5. Security — change dashboard username/password.

Implementation notes:

- ESPAsyncWebServer, not a synchronous `WebServer`
- No heavy Arduino `String` concatenation in handlers; prefer `snprintf` into fixed buffers or PROGMEM HTML
- Log free heap after each request and each protection cycle

## 8. Buzzer

GPIO 21, LEDC, non-blocking. Protection queues a tone id; `loop()` advances the sequencer.

| Function | Pattern |
|---|---|
| `tonePowerUp()` | Three ascending beeps |
| `toneFault()` | Repeating 1 kHz / silence until Reset or Stop |
| `toneMotorAdded()` | Two short mid beeps |
| `toneMotorStarted()` | One rising chirp |
| `toneMotorStopped()` | One falling chirp |
| `toneClick()` | 20 ms tick |

## 9. SD logging

SPI (not SDIO), CS GPIO 10. RoboticsBD 3.3 V breakout, no CD pin. Presence = successful `SD.begin()`.

Mount failure: set `sd_ok = false`, Serial warning, never touch `File` objects, skip writes. Dashboard and protection continue. The 32-entry RAM ring is always written; the web Log page serves it with `ram_only:true` when SD is missing.

Log file `/faults.csv`. Header: `uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA`

Types: `I2T`, `STALL`, `SENSOR_FAULT`, `UNDERVOLT`, `OVERVOLT`, `NO_CURRENT`.

3-phase current-at-fault is the RMS of the phase that crossed the threshold. `voltage_V` is the DC bus voltage at the fault (0 for AC motors). DC rows fill `power_W`, AC rows fill `power_VA`; the other stays blank. Older 4- or 5-column CSVs still parse.

Clear log from UI truncates the file. Export is a download of the same CSV.

No motor configuration is ever stored on SD.

## 10. Heap safety

- Fixed-size motor array, fixed command queue, reserved print buffers
- No `String +=` in the protection path or in request handlers
- SD failure must not leave a half-init filesystem object; `sd_ok` gates every call
- Enable heap poisoning for development (Arduino IDE Core Debug Level = Debug, and `CONFIG_HEAP_POISONING_COMPREHENSIVE` if a custom sdkconfig is used)
- Serial log of `ESP.getFreeHeap()` after each web request and each protection cycle

## 11. Libraries (assumed; Arduino IDE Library Manager)

These were not named by the user; they are the natural Arduino-IDE match for the locked architecture:

- Arduino-ESP32 3.x board package, board = "ESP32S3 Dev Module", PSRAM = "OPI PSRAM", Flash = 16 MB
- `ESPAsyncWebServer` + `AsyncTCP` (ESP32Async / compatible Arduino-ESP32 3.x build)
- `Adafruit ADS1X15` + `Adafruit BusIO` (DC voltage)
- `U8g2` (SH1106 local panel)
- `Adafruit NeoPixel` (onboard WS2812 on GPIO 48)
- Built-in: `WiFi`, `Preferences` (NVS), `SD`, `SPI`, `FS`, `Wire`, `esp32-hal-ledc`

If a library fails to compile on Arduino-ESP32 3.x, swap to the maintained ESP32Async fork without changing the HTTP API.

## 12. Error handling

| Failure | Behaviour |
|---|---|
| SD missing/unformatted | Boot, protect, serve UI; log page = 32-entry RAM ring (`ram_only`) |
| NVS corrupt | Empty motor list; Serial warning |
| Sensor fault | Trip that motor |
| ADS1115 missing (DC) | DC Start rejected; a missing half leaves its AC channels usable |
| Auth missing/wrong | HTTP 401, no motor commands |
| Insufficient free channels | Add-motor rejected |
| Calibrate while a motor runs | Rejected |
| Delete while Running/Cooling | Rejected |

## 13. Testing (firmware-level, no hardware lab in this repo)

- State-machine unit-style tests on host are out of scope for Arduino IDE; keep functions pure enough to reason about (`energy_update()`, `active_step()`, `should_trip()`).
- Serial boot banner checklist
- SoftAP join + `/login` then dashboard 200; unauthenticated `/api` returns 401
- Simulated RMS via a test hook compiling only if `MPS_TEST_HOOKS` is defined (optional, later)

## 14. Out of scope

- Station-mode Wi-Fi, WiFiManager, DDNS, NTP, cloud relay
- Mobile-responsive UI
- Cellular
- Hardware schematic / PCB
- AC voltage sensing (ZMPT101B) — descoped; AC uses a manual rated-voltage field only
- Git commit (user will request it)

## 15. Assumptions vs escalated questions

See `AGENTS.md` section "Open questions and assumptions". Nothing safety-relevant in this spec was guessed after the review: relay polarity, trip math, sensor-fault behaviour, and auth were all confirmed.
