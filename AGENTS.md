# AGENTS.md — ESP32-S3 Programmable Motor Protection Firmware

Source of truth for this repository. Update at the end of every work session or milestone.

## Project summary

University firmware project: replace a bimetallic thermal overload relay with an ESP32-S3 I²t motor protector.

- Up to 8 channels = 8 ACS712-30A sensors + 8 relays
- 1-phase motor = 1 channel; 3-phase motor = 3 linked channels, one dashboard row
- Classic I²t energy model, independent stall trip, sensor-fault trip
- Desktop web dashboard on a fixed SoftAP (`MPS-505` / `mps50005`)
- Themed `/login` page + HttpOnly session cookie (`mps` / `mps500` default)
- Motor config in NVS; SD card used only for fault logs
- Arduino IDE sketch, dual FreeRTOS task split

Hardware is treated as already wired. This repo is firmware only.

Design spec: `docs/superpowers/specs/2026-09-02-motor-protection-firmware-design.md`

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

Analog: ACS712-30A (66 mV/A) → 10k/15k divider (×0.6) → ~39.6 mV/A at the ADC. Zero-current voltage is **calibrated**, never assumed 1.5 V.

All GPIO numbers live only in `MotorProtection/config_pins.h`.

## Motor / channel data model

NVS namespace `mps`, blob of `MotorRecord[8]`. Never stored on SD.

Motor record: name, phase count (1 or 3), assigned channel indices, AC/DC, mains Hz (50/60 if AC), operating current `In`, stall current, cooling time, auto-restart, per-channel relay polarity (default active-HIGH), N ≤ 8 steps of `(k × In, t_trip)`.

Runtime (RAM): per-channel zero ADC, last RMS, and I²t energy; per-motor status, uptime, fault count. Dashboard thermal % = hottest phase (`100 * E / E_trip`).

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

Sensor fault (stuck ADC, Vadc out of `[0.05, 3.05]` V, or |I| > 40 A) → trip `SENSOR_FAULT` immediately. Auto-restart still applies.

On trip: de-energize that motor's relay(s), `toneFault()`, append SD log if mounted, enter Cooling. After cooling: auto-restart if enabled, else Fault. Reset from UI clears Fault/Cooling to Stopped and silences the buzzer.

Fail-safe: all relays OFF in `setup()` before Wi-Fi/tasks. Protection never depends on SD.

## Current implementation status

**v1 firmware written** in `MotorProtection/` (Arduino IDE sketch). Not compiled here (no Arduino-ESP32 toolchain in this environment).

| Module | File | Status |
|---|---|---|
| Pins / limits / types | `config_pins.h`, `config_limits.h`, `types.h` | done |
| Relays fail-safe | `relays.cpp` | done — OFF in `setup()` before Wi-Fi |
| Buzzer named tones | `buzzer.cpp` | done — non-blocking LEDC sequencer |
| Sensing / RMS | `sensing.cpp` | done — calibrate + true RMS / DC mean |
| I²t / stall / sensor-fault | `protection.cpp` | done — dedicated FreeRTOS task, prio 5, core 1 |
| NVS motor store | `motor_store.cpp` | done — blob + login credentials |
| SoftAP | `net_ap.cpp` | done — `MPS-505` / `mps50005` (WPA2 needs ≥ 8 chars) |
| SD fault log | `sd_log.cpp` | done — optional mount, no-op if missing |
| Web dashboard | `web.cpp`, `web_html.h` | done — themed login, session cookie, 5 pages, ~1 s poll |
| Sketch entry | `MotorProtection.ino` | done |

Boot Serial prints AP SSID/password/IP, dashboard login, and free heap. Heap is also logged after each web response and ~1 s in the protection task. First boot `NVS: no motor blob — starting empty` is expected. Bench heap after login ~207 kB.

Protection samples ADC **outside** the status mutex (copy channel zeros, sample, then re-lock to apply I²t). Calibrate copies channels out, runs ADC, copies zeros back — never holds the mutex across `analogRead`. Buzzer uses Arduino-ESP32 3.x LEDC: `ledcAttach(pin,freq,res)`, `ledcWrite(pin,duty)`, `ledcChangeFrequency(PIN_BUZZER, freq, 10)` (3-arg; 2-arg does not compile on 3.x).

Arduino IDE: board **ESP32S3 Dev Module**, Flash **16 MB**, PSRAM **OPI PSRAM**, Core Debug Level **Debug**. Libraries: ESPAsyncWebServer + AsyncTCP (ESP32Async forks).

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
- Fault CSV path `/faults.csv` with `uptime_ms,motor,type,current_A`
- Buzzer frequencies as named functions in the design spec (1 kHz fault, short chirps)
- Live status poll interval ≈ 1 s
- Security page exists to change dashboard login credentials (invalidates session)
- Core assignment: Wi-Fi on core 0, protection + loop on core 1
- `MPS_TEST_HOOKS` compile flag is optional and not required for v1

### Escalated, then closed

- DDNS/STA dropped at user request → SoftAP only
- Board name corrected from generic WROOM-1 / N15R8 to N16R8
- AP credentials changed from MAC-derived to `MPS-505` / `mps50005` (was `mps505`, too short for WPA2)

### Still open (none blocking v1)

- Exact Arduino IDE board-menu checkboxes beyond Flash 16 MB / OPI PSRAM (USB CDC on boot, etc.) — documented as Dev Module defaults in `docs/USER_MANUAL.md`
- Physical SD card on first bench test — firmware treats missing card as valid (`SD: mount failed — fault log unavailable`)

## Bench notes (do not regress)

- AP password `mps505` (6 chars) failed WPA2 association — must stay `mps50005` (≥ 8).
- HTTP Basic Auth was replaced at user request; dashboard is `/login` + cookie `mps_sess` (HttpOnly, SameSite=Strict, 8 h). Dashboard login `mps` / `mps500` is **not** the AP password.
- Holding the protection mutex across ADC sample windows starved the web snapshot — sample off-mutex.
- Missing SD is a warning, not a boot failure.

## Next planned steps

1. Flash this build — confirm `/login` then dashboard (no browser Basic Auth prompt)
2. Inject current / short a sense pin to verify I²t, stall, and SENSOR_FAULT trips
3. Confirm missing-SD path (log page banner) and present-SD CSV write

Last firmware commit: `3b7a424`. Docs (`AGENTS.md`, `docs/USER_MANUAL.md`) follow.
