# AGENTS.md — ESP32-S3 Programmable Motor Protection Firmware

Source of truth for this repository. Update at the end of every work session or milestone.

## Project summary

University firmware project: replace a bimetallic thermal overload relay with an ESP32-S3 I²t motor protector.

- Up to 8 channels = 8 ACS712-30A sensors + 8 relays + 8 DC voltage taps (2× ADS1115)
- 1-phase motor = 1 channel; 3-phase motor = 3 linked channels, one dashboard row
- Classic I²t energy model, independent stall trip, sensor-fault trip
- Live DC voltage (ADS1115) with optional UV/OV trip; AC rated voltage is a manual NVS field only
- Desktop web dashboard on a fixed SoftAP (`MPS-505` / `mps50005`)
- Themed `/login` page + HttpOnly session cookie (`mps` / `mps500` default)
- Motor config in NVS; SD card used only for fault logs
- Arduino IDE sketch, dual FreeRTOS task split

Hardware is treated as already wired. This repo is firmware only.

Design specs: `docs/superpowers/specs/2026-09-02-motor-protection-firmware-design.md` (overall, updated for v2), `docs/superpowers/specs/2026-09-13-dc-voltage-sensing-design.md` (DC voltage / rated-AC delta), and `docs/superpowers/specs/2026-09-14-power-display-design.md` (power display / logging delta).

License: MIT (`LICENSE`). Project overview: `README.md`. User-facing guide: `docs/USER_MANUAL.md`.

## Hardware / pin table

Board: **ESP32-S3-N16R8** (16 MB flash, 8 MB octal PSRAM). Arduino IDE: "ESP32S3 Dev Module", Flash 16 MB, PSRAM OPI.

Avoid GPIO 0/3/45/46 (strapping), 19/20 (USB-JTAG), 43/44 (UART0 Serial).

| Function | GPIO | Notes |
|---|---|---|
| I-sense CH0–CH7 | 1, 2, 4, 5, 6, 7, 8, 9 | ADC1 only |
| Relay CH0–CH7 | 15, 16, 17, 18, 38, 39, 40, 41 | Active-HIGH default, OFF=LOW |
| SD MOSI / MISO / SCK / CS | 11 / 13 / 12 / 10 | SPI, 3.3 V breakout, not SDIO |
| Buzzer | 21 | Passive, LEDC PWM |
| I²C SDA / SCL | 14 / 42 | Explicit `Wire.begin(14, 42)` — not default 8/9 |

DC voltage: 2× ADS1115 over that I²C bus. 0x48 (ADDR→GND) = CH0–3 AIN0–3; 0x49 (ADDR→VDD) = CH4–7 AIN0–3. Gain `GAIN_ONE` (±4.096 V). Divider R1=180 kΩ / R2=10 kΩ (scale 19), 0–50 V → ~2.63 V. Tap is each motor **terminal downstream of its relay**, not the shared bus.

Analog current: ACS712-30A (66 mV/A) → 10k/15k divider (×0.6) → ~39.6 mV/A at the ADC. Zero-current and DC zero-voltage are **calibrated**, never assumed.

All GPIO numbers live only in `MotorProtection/config_pins.h`.

## Motor / channel data model

NVS namespace `mps`, blob of `MotorRecord[8]`. Never stored on SD.

Motor record: name, phase count (1 or 3), assigned channel indices, AC/DC, mains Hz (50/60 if AC), rated AC voltage (AC only, static), operating current `In`, stall current, cooling time, auto-restart, per-channel relay polarity (default active-HIGH), N ≤ 8 steps of `(k × In, t_trip)`, DC undervoltage / overvoltage (0 = that trip disabled).

Runtime (RAM): per-channel zero ADC, last RMS, I²t energy, voltage zero and last V; per-motor status, uptime, fault count, latest power and session energy. Dashboard thermal % = hottest phase (`100 * E / E_trip`). Live V is DC only. Power is DC true `W` (`V × I`), AC apparent `VA` (`V_rated × I_rms`, or `(V_rated/√3) × ΣI` for 3-phase). Energy (`Wh`/`VAh`) is RAM only, accumulates while Running, shows `0.0` otherwise, resets on Start and reboot. Never stored in NVS.

NVS schema **2** (v1 blob is discarded on first boot of this build).

Statuses: Stopped, Running, Fault, Cooling.

3-phase: one motor, three channels; any phase trips all three relays; UI shows one row.

## I²t logic (precise)

Sampling: ≥ 32 samples over ≥ 1 AC cycle (20 ms @ 50 Hz, 16.67 ms @ 60 Hz). True RMS. DC uses a 20 ms mean of |i|, not RMS.

Running and `I_rms >= k_min × In`:

- `E += I_rms² × dt`  (A²s)
- Active step = highest k with `I_rms >= k × In`
- Trip `I2T` when `E >= (k × In)² × t_trip`

Running and below pickup: `E` decays toward 0 over `cooling_s`.

Stall: `I_rms >= stall_amps` on the next window → trip `STALL` immediately.

Sensor fault (stuck ADC, Vadc out of `[0.05, 3.05]` V, |I| > 40 A, missing ADS1115 on a running DC channel, |Vadc| > 4 V, or |Vbus| > 55 V) → trip `SENSOR_FAULT` immediately. Auto-restart still applies.

DC only, after 250 ms of Running (relay just closed): `Vbus < uv_volts` (if uv > 0) → `UNDERVOLT`; `Vbus > ov_volts` (if ov > 0) → `OVERVOLT`. Same Cooling / auto-restart / Reset as I²t. AC rated voltage is never compared.

On trip: de-energize that motor's relay(s), `toneFault()`, append SD log if mounted, enter Cooling. After cooling: auto-restart if enabled, else Fault. Reset from UI clears Fault/Cooling to Stopped and silences the buzzer.

Fail-safe: all relays OFF in `setup()` before Wi-Fi/tasks. Protection never depends on SD.

## Current implementation status

**v1 + DC voltage / AC rated-V field + power display** in `MotorProtection/` (Arduino IDE sketch). Not compiled here (no Arduino-ESP32 toolchain in this environment).

| Module | File | Status |
|---|---|---|
| Pins / limits / types | `config_pins.h`, `config_limits.h`, `types.h` | done — I²C 14/42, schema 2, UV/OV + rated AC |
| Relays fail-safe | `relays.cpp` | done — OFF in `setup()` before Wi-Fi |
| Buzzer named tones | `buzzer.cpp` | done — non-blocking LEDC sequencer |
| Sensing / RMS | `sensing.cpp` | done — current only, calibrate + true RMS / DC mean |
| DC voltage ADS1115 | `voltage.cpp` | done — Adafruit ADS1X15, GAIN_ONE, off-mutex sample |
| I²t / stall / UV / OV / sensor-fault / power | `protection.cpp` | done — dedicated FreeRTOS task, prio 5, core 1; power from same pass, RAM-only energy |
| NVS motor store | `motor_store.cpp` | done — blob + login credentials |
| SoftAP | `net_ap.cpp` | done — `MPS-505` / `mps50005` (WPA2 needs ≥ 8 chars) |
| SD fault log | `sd_log.cpp` | done — optional mount, no-op if missing; power_W / power_VA columns |
| Web dashboard | `web.cpp`, `web_html.h` | done — themed login, session cookie, live DC V, power + session energy, AC/DC form |
| Sketch entry | `MotorProtection.ino` | done |

Boot Serial prints AP SSID/password/IP, dashboard login, and free heap. Heap is also logged after each web response and ~1 s in the protection task. First boot `NVS: no motor blob — starting empty` is expected. Bench heap after login ~207 kB.

Protection samples current ADC and ADS1115 **outside** the status mutex (copy channel zeros, sample, then re-lock to apply I²t / UV / OV). Calibrate copies channels out, runs ADC + ADS, copies zeros back — never holds the mutex across `analogRead` or I²C. Adafruit BusIO's `ads.begin()` calls `Wire.begin()` with no pins; firmware re-binds `Wire.begin(14, 42)` before and after each `begin()`. Buzzer uses Arduino-ESP32 3.x LEDC: `ledcAttach(pin,freq,res)`, `ledcWrite(pin,duty)`, `ledcChangeFrequency(PIN_BUZZER, freq, 10)` (3-arg; 2-arg does not compile on 3.x).

Arduino IDE: board **ESP32S3 Dev Module**, Flash **16 MB**, PSRAM **OPI PSRAM**, Core Debug Level **Debug**. Libraries: ESPAsyncWebServer + AsyncTCP (ESP32Async forks), Adafruit ADS1X15 + Adafruit BusIO.

User-facing guide (pins, wiring, libraries, dashboard, I²t math): `docs/USER_MANUAL.md`.

## Open questions and assumptions

### Resolved with the user (do not re-open without asking)

- Arduino IDE (not PlatformIO, not ESP-IDF)
- Auth: themed login page + session cookie; default `mps` / `mps500` (was HTTP Basic Auth; user requested custom page)
- Sensor fault = trip immediately (not latch, not alarm-only)
- ACS712-30A 66 mV/A
- Relay default active-HIGH, OFF=LOW
- Mains frequency selectable per motor
- Classic I²t energy (not inverse-time interpolation, not independent step timers)
- Dual-task architecture
- SoftAP only — no STA, no WiFiManager, no DDNS
- AP SSID/password **fixed** `MPS-505` / `mps50005` (not MAC-derived; WPA2 min 8 chars)
- Stall = immediate next RMS window
- I²t decays over cooling time while below pickup
- Max 8 protection steps
- Board ESP32-S3-N16R8
- Delete motor only from Stopped/Fault
- Timestamps = `millis()` uptime (no NTP)

### Assumptions made unilaterally (not safety-relevant; change if you object)

- Arduino-ESP32 3.x + ESPAsyncWebServer + AsyncTCP
- NVS namespace `mps`, single blob key `motors`
- RMS sample count = 32 minimum
- Sensor-fault |I| cap = 40 A
- ADC attenuation = 11 dB
- Channel allocation = lowest free indices
- Thermal % = `100 * E / E_trip` of the active step
- Fault CSV path `/faults.csv` with `uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA`; old 4/5-column rows still parse
- Power/energy are RAM only in `MotorRuntime`; no NVS write, no schema bump (stays 2). Energy resets on Start and reboot
- AC power is apparent `VA` at rated V; power factor is not measurable and must never be shown as `W`
- I²C SDA/SCL = GPIO 14 / 42; ADS1115 data rate 250 SPS; UV/OV grace 250 ms after DC Start
- NVS schema 2; flashing this build drops a v1 motor blob (size/schema mismatch → empty list)
- Buzzer frequencies as named functions in the design spec (1 kHz fault, short chirps)
- Live status poll interval ≈ 1 s
- Security page exists to change dashboard login credentials (invalidates session)
- Core assignment: Wi-Fi on core 0, protection + loop on core 1
- `MPS_TEST_HOOKS` compile flag is optional and not required for v1

### Escalated, then closed

- DDNS/STA dropped at user request → SoftAP only
- Board name corrected from generic WROOM-1 / N15R8 to N16R8
- AP credentials changed from MAC-derived to `MPS-505` / `mps50005` (was `mps505`, too short for WPA2)
- AC voltage sensing (ZMPT101B) considered and descoped — AC uses a manual rated-voltage field only; do not re-open without asking

### Still open (none blocking v1)

- Exact Arduino IDE board-menu checkboxes beyond Flash 16 MB / OPI PSRAM (USB CDC on boot, etc.) — documented as Dev Module defaults in `docs/USER_MANUAL.md`
- Physical SD card on first bench test — firmware treats missing card as valid (`SD: mount failed — fault log unavailable`)

## Bench notes (do not regress)

- AP password `mps505` (6 chars) failed WPA2 association — must stay `mps50005` (≥ 8).
- HTTP Basic Auth was replaced at user request; dashboard is `/login` + cookie `mps_sess` (HttpOnly, SameSite=Strict, 8 h). Dashboard login `mps` / `mps500` is **not** the AP password.
- Holding the protection mutex across ADC sample windows starved the web snapshot — sample off-mutex. Power reuses those same off-mutex samples; never add a second ADC/ADS pass.
- Missing SD is a warning, not a boot failure.

## Next planned steps

1. Flash this build — confirm `/login` then dashboard (no browser Basic Auth prompt)
2. Confirm Serial `ADS1115 0x48/0x49: ok` (or `not found` if unpopulated); DC Start disabled without the chip for that motor
3. Inject current / short a sense pin to verify I²t, stall, and SENSOR_FAULT trips
4. DC: apply voltage downstream of a closed relay; confirm live V and UV/OV trips (0 = disabled)
5. Confirm missing-SD path (log page banner) and present-SD CSV write
6. Power: DC known load vs bench meter; AC hand-check `V_rated × I_rms`; 3-phase current-unbalance check; trip CSV carries `power_W` / `power_VA`; energy reads `0.00` after reboot

Last firmware: power calc + dashboard display + fault-log power columns on top of DC-voltage build (`12941fb`). Docs (`AGENTS.md`, `README.md`, `docs/USER_MANUAL.md`, all design specs) updated.
