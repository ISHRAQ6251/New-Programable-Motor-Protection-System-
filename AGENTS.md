# AGENTS.md — ESP32-S3 Programmable Motor Protection Firmware

Source of truth for this repository. Update at the end of every work session or milestone.

## Project summary

University firmware project: replace a bimetallic thermal overload relay with an ESP32-S3 I²t motor protector.

- Up to 8 channels = 8 ACS712-30A sensors + 8 relays + 8 DC voltage taps (2× ADS1115)
- 1-phase motor = 1 channel; 3-phase motor = 3 linked channels, one dashboard row
- Classic I²t energy model, independent stall trip, optional stall recovery (jam release), sensor-fault trip, `NO_CURRENT`
- Live DC voltage (ADS1115) with optional UV/OV trip (4-sample average + LPF, 750 ms grace); AC rated voltage is a manual NVS field only
- User-selectable current channels (Auto or CH0–CH7) and per-motor DC voltage sense channel (`VCH_SAME` or 0–7)
- Power: DC true `W`; AC apparent `VA` at rated V; energy RAM-only
- Desktop web dashboard on a fixed SoftAP (`MPS-505` / `mps50005`)
- Local panel: SH1106 128x64 OLED on the shared ADS1115 I2C bus (`Wire` GPIO 14/42) + KY-040 encoder (rotate / short / long press) + onboard WS2812 status LED (GPIO 48), with buzzer feedback
- Themed `/login` page + HttpOnly session cookie (`mps` / `mps500` default)
- Motor config in NVS; SD card used for persistent fault logs; 32-entry RAM ring feeds the web log page when SD is missing
- Arduino IDE sketch, three FreeRTOS tasks (Wi-Fi/UI core 0, protection/loop core 1)

Hardware is treated as already wired. This repo is firmware only.

Design specs: `docs/superpowers/specs/2026-09-02-motor-protection-firmware-design.md` (overall, updated for v2), `docs/superpowers/specs/2026-09-13-dc-voltage-sensing-design.md` (DC voltage / rated-AC delta), `docs/superpowers/specs/2026-09-14-power-display-design.md` (power display / logging), `docs/superpowers/specs/2026-09-20-oled-encoder-local-panel-design.md` (local OLED + encoder panel). Roadmap: `docs/ROADMAP.md`. I²C bench: `docs/I2C_TROUBLESHOOTING.md`.

License: MIT (`LICENSE`). Project overview: `README.md`. User-facing guide: `docs/USER_MANUAL.md`.

## Hardware / pin table

Board: **ESP32-S3-N16R8** (16 MB flash, 8 MB octal PSRAM). Arduino IDE: "ESP32S3 Dev Module", Flash 16 MB, PSRAM OPI.

GPIO 22–25 do not exist on this chip at all (not physical pins). GPIO 26–37 are reserved for flash/octal PSRAM on this N16R8 module. GPIO 0/45 remain fully off-limits (boot mode / flash voltage). GPIO 3 is ENC_SW (strapping-safe with the KY-040 pull-up). GPIO 46 is input-only/strapping (ROM extra boot-log text only) and is used here as ENC_B (DT). GPIO 43/44 are UART0 Serial and stay untouched. GPIO 19/20 are native USB D-/D+. This board uses native USB for serial (NOT a UART bridge). GPIO 19 must never be assigned as a GPIO — doing so crashes the board and breaks serial. GPIO 48 is the onboard WS2812 (not an encoder pin).

| Function | GPIO | Notes |
|---|---|---|
| I-sense CH0–CH7 | 1, 2, 4, 5, 6, 7, 8, 9 | ADC1 only |
| Relay CH0–CH7 | 15, 16, 17, 18, 38, 39, 40, 41 | Active-LOW default, OFF=HIGH |
| SD MOSI / MISO / SCK / CS | 11 / 13 / 12 / 10 | SPI, 3.3 V breakout, not SDIO |
| Buzzer | 21 | Passive, LEDC PWM |
| I²C SDA / SCL | **14 / 42** | Shared ADS1115 + SH1106. `Wire.begin(14, 42)` once via `i2cInitOnce()` — **not** default 8/9. Serialized by `s_i2c_mu`. |
| Encoder CLK / DT / SW | 47 / 46 / 3 | KY-040, plain `INPUT` (module has onboard pull-ups). GPIO 46 is ENC_B (DT) only. GPIO 3 is ENC_SW (strapping-safe with pull-up). GPIO 19 is USB D- and must never be used as GPIO. |
| Status LED | 48 | Onboard WS2812, one-wire. Never take `s_i2c_mu`. |

DC voltage: 2× ADS1115 over that I²C bus. **0x48** (ADDR→GND) = CH4–7 AIN0–3; **0x49** (ADDR→VDD) = CH0–3 AIN0–3. Gain `GAIN_ONE` (±4.096 V). Divider R1=150 kΩ / R2=10 kΩ (scale 16), 0–50 V → ~3.13 V. Tap is each motor **terminal downstream of its relay**, not the shared bus.

Analog current: ACS712-30A (66 mV/A) → 10k/15k divider (×0.6) → ~39.6 mV/A at the ADC. Zero-current and DC zero-voltage are **calibrated**, never assumed.

All GPIO numbers live only in `MotorProtection/config_pins.h`.

## Motor / channel data model

NVS namespace `mps`, blob of `MotorRecord[8]`. Never stored on SD.

Motor record: name, phase count (1 or 3), assigned channel indices (Auto = lowest free, or pin `ch0`/`ch1`/`ch2` 0–7), AC/DC, mains Hz (50/60 if AC), rated AC voltage (AC only, static), operating current `In`, stall current, starting current (`start_current`, 0 = disabled), inrush duration (`icd_ms`, 0 = disabled), cooling time, auto-restart, stall recovery (jam release, default off), per-channel relay polarity (default active-LOW), N ≤ 8 steps of `(k × In, t_trip)`, DC undervoltage / overvoltage (0 = that trip disabled), optional `voltage_channel` (0–7 or `VCH_SAME` 0xFF = same as current CH0).

Runtime (RAM): per-channel zero ADC, last RMS, I²t energy, voltage zero and last V; per-motor status, uptime, fault count, latest power and session energy. Dashboard thermal % = hottest phase (`100 * E / E_trip`). Live V is DC only. Power is DC true `W` (`V × I`), AC apparent `VA` (`V_rated × I_rms`, or `(V_rated/√3) × ΣI` for 3-phase). Energy (`Wh`/`VAh`) is RAM only, accumulates while Running, shows `0.0` otherwise, resets on Start and reboot. Never stored in NVS.

NVS schema **4** (older blobs are discarded on first boot of this build).

Statuses: Stopped, Running, Fault, Cooling.

3-phase: one motor, three channels; any phase trips all three relays; UI shows one row.

## I²t logic (precise)

Sampling: ≥ 32 samples over ≥ 1 AC cycle (20 ms @ 50 Hz, 16.67 ms @ 60 Hz). True RMS. DC uses a 20 ms mean of |i|, not RMS.

Running and `I_rms >= k_min × In`:

- `E += I_rms² × dt`  (A²s)
- Active step = highest k with `I_rms >= k × In`
- Trip `I2T` when `E >= (k × In)² × t_trip`

Running and below pickup: `E` decays toward 0 over `cooling_s`.

Stall: `I_rms >= stall_amps` on the next window → trip `STALL` immediately, unless stall recovery is enabled. Then, after `JAM_RELEASE_MIN_RUN_MS` of Running: de-energize 300 ms, re-energize, wait 500 ms, re-check. Up to 4 pulses (`JAM_RELEASE_MAX`). Success (current `< stall_amps`) resumes Running and zeros `jam_count` (does **not** clear the auto-restart counter). Exhaustion trips `STALL` and enters Cooling as usual. Jam attempts are separate from auto-restart. While `jam_phase != JAM_IDLE`, skip stall / UV / OV / `NO_CURRENT` / `SENSOR_FAULT`; keep I²t accumulation. No extra grace after jam ends — `JAM_WAIT` is the settle window. During the configured inrush window (`icd_ms` > 0 and `start_current` > 0), the effective stall threshold is `start_current` instead of `stall_amps`.

Sensor fault, while `jam_phase == JAM_IDLE`. Current path (stuck ADC AC or DC, mean Vadc outside `[0.05, 3.05]` V, or |I| > 40 A) → trip `SENSOR_FAULT` on that sample. DC voltage path (missing ADS1115, I²C timeout / `present=0`, `|Vadc| > 4` V, or `|Vbus| > 55` V) → trip `SENSOR_FAULT` only after 3 consecutive voltage faults within 1 s (`SENSOR_FAULT_MIN_COUNT` / `SENSOR_FAULT_WINDOW_MS`). A single voltage glitch is ignored. Auto-restart still applies. Trip order in one window: current `SENSOR_FAULT`, `STALL`, voltage `SENSOR_FAULT` (debounced), `OV`, `UV`, `I2T`.

Running and max phase current `< 0.05 × In` for 2 s → trip `NO_CURRENT` (broken sense wire / open winding / relay that never closed).

DC only, after 750 ms of Running (relay just closed): 4-sample ADS average then 0.9/0.1 LPF on `Vbus`; `Vbus < uv_volts` (if uv > 0) → `UNDERVOLT`; `Vbus > ov_volts` (if ov > 0) → `OVERVOLT`. Same Cooling / auto-restart / Reset as I²t. AC rated voltage is never compared. `VOLT:` Serial every 2 s while Running.

On trip: de-energize that motor's relay(s), `toneFault()`, append SD log if mounted, enter Cooling. After cooling: auto-restart if enabled **and** consecutive trips `< 3`, else Fault. A trip-free run of 10 min clears the restart counter; Start and Reset also clear it. Reset from UI clears Fault/Cooling to Stopped and silences the buzzer.

Fail-safe: all relay GPIOs forced to the de-energized level at the top of `setup()` (before `Serial.begin` / 200 ms delay) — HIGH for the default active-LOW polarity — then `relaysBegin()`. Protection task feeds a 5 s Task WDT; a hang resets the MCU and `setup()` drops relays. Protection never depends on SD. Sample each running motor then trip immediately (not after every other channel). `dt` credited up to 5 s after a task stall.

## Current implementation status

**v2** in `MotorProtection/` (Arduino IDE sketch). Not compiled here (no Arduino-ESP32 toolchain in this environment).

| Module | File | Status |
|---|---|---|
| Pins / limits / types | `config_pins.h`, `config_limits.h`, `types.h` | done — I²C 14/42, schema 4, UV/OV + rated AC, jam-release, `voltage_channel` / `VCH_SAME`, `start_current` / `icd_ms`, VoltageSample / VoltageDiagnostic |
| Relays fail-safe | `relays.cpp` | done — GPIO HIGH at top of `setup()` (active-LOW default, OFF=HIGH) then `relaysBegin()`; polarity printed at de-energize |
| Buzzer named tones | `buzzer.cpp` | done — non-blocking LEDC sequencer |
| Sensing / RMS | `sensing.cpp` | done — current only, calibrate + true RMS / DC mean; stuck-ADC Serial |
| DC voltage ADS1115 | `voltage.cpp` / `voltage.h` | done — `i2cInitOnce()`, `voltageReprobe()`, no `Wire.end()`, 128 SPS asynchronous conversion wait with 35 ms timeout, `s_i2c_mu`, 4-sample average, per-channel `VOLT_DIAG` ring |
| I²t / stall / jam release / UV / OV / sensor-fault / NO_CURRENT / power | `protection.cpp` | done — prio 5, core 1; 5 s Task WDT; per-motor sample-then-trip; 3-restart cap; optional jam release (4× 300/500 ms); 32-entry RAM trip ring; Calibrate calls `voltageReprobe()` off-mutex; 0.9/0.1 V LPF; 750 ms UV/OV grace; `VOLT:` log; dedicated `voltage_channel`; current-path `SENSOR_FAULT` immediate; voltage-path debounce 3/1 s; jam skips `SENSOR_FAULT`; inrush `start_current`/`icd_ms` |
| NVS motor store | `motor_store.cpp` | done — blob + login credentials; load sanitizes; stall must exceed In/steps; cooling ≤ 86400 s; AP-password auth_pass restored to `mps500`; manual `ch0`–`ch2` or Auto; `voltage_channel` |
| SoftAP | `net_ap.cpp` | done — `MPS-505` / `mps50005` (WPA2 needs ≥ 8 chars) |
| SD fault log | `sd_log.cpp` | done — optional mount, no-op if missing; power_W / power_VA columns |
| Web dashboard | `web.cpp`, `web_html.h` | done — themed login, session cookie, live DC V, power + session energy, AC/DC form, stall-recovery checkbox, jam indicator; `s_json` 8192 B; `/api/motor/add`; Start gate via `protectionCanStart()`; log page uses SD or 32-entry RAM ring (`ram_only`); Add/Edit `ch0`–`ch2` + `vch` dropdowns + `startcur`/`icd`; Export CSV falls back to RAM ring |
| Local panel | `ui.cpp`, `ui.h`, `ui_icons.h` | done — `uiTask` core 0; SH1106 on Wire; KY-040 on 47/46/3; WS2812 on GPIO 48; 3-frame status icons; boot splash, Home, Per-motor, Fault Log, Diagnostics, Firmware Info, Network Info; `toneBack()` |
| Sketch entry | `MotorProtection.ino` | done — relay GPIOs driven de-energized first (HIGH for active-LOW default); mutex/queue init (`voltageI2cMutexInit` / `buzzerMutexInit` / `motorStoreMutexInit` / `protectionMutexInit` / `webMutexInit`) runs before any `begin()`; `uiBegin()` starts `uiTask` only after those handles exist |

Boot Serial prints AP SSID/password/IP, dashboard login (`mps` / `mps500`, **not** AP pass), and free heap (`HEAP:`). Heap is also logged after each web response and ~1 s in the protection task. First boot `NVS: no motor blob — starting empty` is expected.

Protection samples current ADC and ADS1115 **outside** the status mutex (copy channel zeros, sample, then re-lock to apply I²t / UV / OV). Calibrate copies channels out, calls `voltageReprobe()` (scan + probe, no `Wire.end()`), then ADC + ADS zeros, copies zeros back. Channel map is fixed: CH0–3 always 0x49, CH4–7 always 0x48. Adafruit BusIO's `ads.begin()` may call `Wire.begin()` with no pins; firmware re-binds `Wire.begin(14, 42)` after each `begin()` and never calls `Wire.end()`. `loop()` prints `I2C: diag ads_ok=[…] vcal=[…]` at most every 5 s when a chip or channel zero is not ready. Buzzer uses Arduino-ESP32 3.x LEDC: `ledcAttach(pin,freq,res)`, `ledcWrite(pin,duty)`, `ledcChangeFrequency(PIN_BUZZER, freq, 10)` (3-arg).

Arduino IDE: board **ESP32S3 Dev Module**, Flash **16 MB**, PSRAM **OPI PSRAM**, Core Debug Level **Debug**. Libraries: ESPAsyncWebServer + AsyncTCP (ESP32Async forks), Adafruit ADS1X15 + Adafruit BusIO, U8g2 (SH1106 panel), Adafruit NeoPixel (GPIO 48 WS2812). The panel runs in `uiTask` on core 0 and reaches motor state only through `protectionSnapshot()` / `protectionCopyLog()`; `setup()` publishes relay / ADS / SD / calibration results to the splash via `uiBootStage()` / `uiBootNote()`.

User-facing guide: `docs/USER_MANUAL.md`.

## File structure

```
MotorProtection/voltage.cpp   i2cInitOnce, voltageReprobe, sample, s_i2c_mu
MotorProtection/voltage.h     VoltageSample, VoltageDiagnostic, voltageReprobe, i2cLock/i2cUnlock
MotorProtection/protection.cpp I²t/stall/jam release/UV/OV/power; protectionCanStart, 32-entry trip ring
MotorProtection/ui.cpp        uiTask: OLED + encoder + WS2812 LED, boot splash + six screens
MotorProtection/ui.h          UiBootStage, uiBegin/uiTask/uiBootStage/uiBootNote
MotorProtection/ui_icons.h    8x8 XBM status (3-frame RUNNING/FAULT/COOLING) + AP icons
MotorProtection/buzzer.cpp    named tones + toneBack, cross-core sequencer mutex
MotorProtection/web.cpp       /api/motor/add after /edit and /del; s_json 8192
MotorProtection/web_html.h    real HTML/CSS/JS dashboard + login
```

## Open questions and assumptions

### Resolved with the user (do not re-open without asking)

- Arduino IDE (not PlatformIO, not ESP-IDF)
- Auth: themed login page + session cookie; default `mps` / `mps500` (was HTTP Basic Auth; user requested custom page)
- Sensor fault = trip (not latch, not alarm-only). Current-path `SENSOR_FAULT` is immediate; DC voltage-path `SENSOR_FAULT` is 3 consecutive faults in 1 s; both skipped while `jam_phase != JAM_IDLE`
- ACS712-30A 66 mV/A
- Relay default active-LOW, OFF=HIGH
- Mains frequency selectable per motor
- Classic I²t energy (not inverse-time interpolation, not independent step timers)
- FreeRTOS task architecture: protection + `loop()` on core 1, Wi-Fi + UI on core 0 (originally two tasks; panel adds `uiTask`)
- SoftAP only — no STA, no WiFiManager, no DDNS
- AP SSID/password **fixed** `MPS-505` / `mps50005` (not MAC-derived; WPA2 min 8 chars)
- Stall = immediate next RMS window
- I²t decays over cooling time while below pickup
- Max 8 protection steps
- Board ESP32-S3-N16R8
- Delete motor only from Stopped/Fault
- Timestamps = `millis()` uptime (no NTP)
- AC voltage sensing (ZMPT101B) descoped

### Assumptions made unilaterally (not safety-relevant; change if you object)

- Arduino-ESP32 3.x + ESPAsyncWebServer + AsyncTCP
- NVS namespace `mps`, single blob key `motors`; auth keys `auth_user` / `auth_pass`
- RMS sample count = 32 minimum
- Sensor-fault |I| cap = 40 A
- ADC attenuation = 11 dB
- Channel allocation = Auto (lowest free) or pinned `ch0`/`ch1`/`ch2` 0–7 from the dashboard
- Thermal % = `100 * E / E_trip` of the active step
- Motor API add endpoint is `/api/motor/add` (never `/api/motor`, which prefixes `/edit` and `/del`)
- Task WDT 5 s on the protection task; NO_CURRENT after 2 s below 0.05×In; auto-restart cap 3, reset after 10 min clean run; `dt` cap 5 s
- Fault CSV path `/faults.csv` with `uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA`; old 4/5-column rows still parse
- Power/energy are RAM only in `MotorRuntime`; no NVS write. Energy resets on Start and reboot
- Jam release: `JAM_RELEASE_MAX = 4`, `JAM_RELEASE_OFF_MS = 300`, `JAM_RELEASE_WAIT_MS = 500`, `JAM_RELEASE_MIN_RUN_MS = 500`; no user-configurable timing; default `stall_recovery = 0`
- AC power is apparent `VA` at rated V; power factor is not measurable and must never be shown as `W`
- I²C SDA/SCL = GPIO 14 / 42; ADS1115 data rate 128 SPS; conversion wait 10 ms then poll to 35 ms; UV/OV grace 750 ms after DC Start; 4-sample ADS average then 0.9/0.1 LPF on `Vbus`
- NVS schema 4; flashing this build drops a v1/v2/v3 motor blob (size/schema mismatch → empty list)
- DC `voltage_channel` is 0–7 or `VCH_SAME` (0xFF = same ADS tap as current CH0); AC forces `VCH_SAME`
- Buzzer frequencies as named functions in the design spec (1 kHz fault, short chirps)
- Live status poll interval ≈ 1 s
- Security page exists to change dashboard login credentials (invalidates session)
- Core assignment: Wi-Fi + UI on core 0, protection + loop on core 1
- `MPS_TEST_HOOKS` compile flag is optional and not required for v1
- Local panel: SH1106 (**not** SSD1306) on the shared ADS1115 bus `Wire` GPIO 14/42 via U8g2 `HW_I2C`; KY-040 on GPIO 47/46/3 with a state-table quadrature decoder (one step per detent) and 40 ms switch debounce, 600 ms long-press threshold; onboard WS2812 on GPIO 48 via Adafruit NeoPixel (one-wire, never `s_i2c_mu`)
- `protectionCanStart()` is the single Start gate shared by the web API and the panel; `protectionCopyLog()` exposes a non-destructive 32-entry RAM trip ring (the SD `protectionPopLog()` queue keeps its single consumer in `loop()`); web log uses that ring when SD is missing (`ram_only`)
- `buzzer.cpp` guards its sequencer with a mutex because `uiTask` (core 0) and `loop()` (core 1) both issue tones; panel Start/Stop success tone comes from protection, not the panel, so it is not doubled
- Panel boot stages are published from `setup()` via `uiBootStage()` / `uiBootNote()`; the splash reveals them, then auto-advances to Home

### Escalated, then closed

- DDNS/STA dropped at user request → SoftAP only
- Board name corrected from generic WROOM-1 / N15R8 to N16R8
- AP credentials changed from MAC-derived to `MPS-505` / `mps50005` (was `mps505`, too short for WPA2)
- AC voltage sensing (ZMPT101B) considered and descoped — AC uses a manual rated-voltage field only; do not re-open without asking

### Still open (none blocking v2)

- Exact Arduino IDE board-menu checkboxes beyond Flash 16 MB / OPI PSRAM (USB CDC on boot, etc.) — documented as Dev Module defaults in `docs/USER_MANUAL.md`
- Physical SD card on first bench test — firmware treats missing card as valid
- Exact U8g2 symbol for the SH1106 `HW_I2C` variant — constructor is `(rotation, reset)`; pins are bound on `Wire`. `_SW_I2C` only if hardware I2C is genuinely unavailable

## Bench notes (do not regress)

- AP password `mps505` (6 chars) failed WPA2 association — must stay `mps50005` (≥ 8).
- HTTP Basic Auth was replaced at user request; dashboard is `/login` + cookie `mps_sess` (HttpOnly, SameSite=Strict, 8 h). Dashboard login `mps` / `mps500` is **not** the AP password.
- Holding the protection mutex across ADC sample windows starved the web snapshot — sample off-mutex. Power reuses those same off-mutex samples; never add a second ADC/ADS pass.
- **Never call `Wire.end()`.** It tears down the driver; a later `begin()` does not fully recover the bus. `i2cInitOnce()` runs once; Calibrate uses `voltageReprobe()`. After `ads.begin()`, re-bind `Wire.begin(14, 42)` because Adafruit BusIO may call `Wire.begin()` with no pins.
- **SDA/SCL swap:** a swapped pair scans as `(none)` or timeouts (`err=5`). Firmware cannot auto-detect a swap. Confirm GPIO 14 = SDA and 42 = SCL on the ADS modules (not ESP32 default 8/9).
- ADS1115 `s_ok[]` is no longer boot-only. Calibrate re-runs `voltageReprobe()` so an ADDR-pin fix is picked up without a power cycle. CH0–3 are served by 0x49 and CH4–7 by 0x48; a swapped ADDR pin shows as the wrong chip missing, not as swapped readings.
- Missing SD is a warning, not a boot failure. Web log then serves the 32-entry RAM ring with `ram_only:true` (lost on reboot); OLED Fault Log uses the same ring.
- Motor API routes must never be prefixes of one another. `/api/motor` collided with `/api/motor/edit` and `/api/motor/del`. Add is `/api/motor/add`; register `/edit` and `/del` first.
- If Serial ever printed `dash pass: mps50005`, that was AP password leaking into NVS `auth_pass`. Load now restores `mps500` when `auth_pass` equals `AP_PASS`.
- The panel shares `Wire` with the ADS1115s (`Wire.begin(14, 42)`), re-bound after `u8g2.begin()` (same BusIO-style precaution). Every raw Wire transaction is held under `s_i2c_mu` only for that call. Never call `Wire.end()`.
- Panel Start must call `protectionCanStart()`; do not re-implement the ADS/vcal check in `ui.cpp`.
- Panel Start/Stop success tones come from protection (`TONE_STARTED` / `TONE_STOPPED`). Do not also call `toneMotorStarted()` / `toneMotorStopped()` from `uiTask`, or the beep doubles.
- KY-040 pins are plain `INPUT`; the module has its own pull-ups. Adding internal pull-ups can fight them.
- GPIO 48 is the onboard WS2812. Encoder is CLK 47 / DT 46 / SW 3. Do not drive GPIO 48 as encoder DT. The LED is one-wire — never take `s_i2c_mu` for it.
- GPIO 19/20 are native USB D-/D+. This board uses native USB for serial (NOT a UART bridge). GPIO 19 must never be assigned as a GPIO — doing so crashes the board and breaks serial.
- GPIO 19 is USB D-. Assigning it as encoder input crashed the board and broke serial. Moved ENC_SW to GPIO 3 (strapping-safe: JTAG source, does not affect boot mode or flash voltage; KY-040 pull-up holds it HIGH at boot).
- `uiBegin()` starts `uiTask`, which calls `protectionSnapshot()` within 160 ms. Create `s_mu` / `s_i2c_mu` / motor-store / buzzer / web mutexes (and protection queues) before any `begin()`. Do not take those handles until `*MutexInit()` has run.
- `uiTask` is not on the Task WDT; only the protection task is.
- Current-path `SENSOR_FAULT` (stuck ADC, Vadc outside `[0.05, 3.05]` V, |I| > 40 A) trips on that sample while `jam_phase == JAM_IDLE` and is checked before stall.
- Voltage-path `SENSOR_FAULT` (missing ADS, I²C timeout, `|Vadc| > 4` V, `|Vbus| > 55` V) requires 3 consecutive faults within 1 s; a single voltage glitch is ignored.
- All sensor-fault checks are skipped while `jam_phase != JAM_IDLE`. A stalled/pulsed motor is low-resistance and voltage/current readings can be temporarily abnormal; `processJamRelease` determines the outcome.
- Inrush current protection: configurable `start_current` and `icd_ms` suppress startup stall false trips while the motor is still accelerating.
- Clear Logs now clears both SD and RAM ring; if either succeeds, the button returns ok.
- OLED rebuilds motor order every UI refresh (160 ms); motor add/edit/delete from web dashboard appears on panel without reboot.
- Web log page serves RAM ring with ram_only:true when SD is missing; JavaScript shows warning banner. Clear stays available; Export remains available and downloads the newest-first RAM ring as `faults.csv`.
- DC voltage noise: ADS average is `V_AVG_SAMPLES` (4) in `voltageSample()`, then 0.9/0.1 LPF on `Vbus` (`ChannelRuntime.v_filt`). UV/OV uses the filtered value after `UV_GRACE_MS` (750). Serial `VOLT: ch=… adc=… zero=… bus=…` every 2 s while Running. Do not trip on a single raw ADS sample.
- Relay GPIOs are forced OUTPUT to the de-energized level at the very top of `setup()`, before `Serial.begin()` (HIGH for default active-LOW). `relaysBegin()` repeats the fail-safe. `RELAY_ACTIVE_HIGH_DEFAULT = 0`.
- Add/Edit channel dropdowns (`ch0`/`ch1`/`ch2`, -1 = Auto) and DC `vch` (`VCH_SAME` or 0–7) are stored in the NVS motor blob (schema 4). Used current channels are disabled in the other dropdowns; voltage taps may be shared.

## Next planned steps

1. Flash this build — confirm `/login` then dashboard (no browser Basic Auth prompt); Serial `dash pass: mps500`
2. Confirm Serial `I2C: initialized SDA=14 SCL=42` then `ADS: 0x48/0x49 ok` (or `not found` if unpopulated); DC Start disabled without the chip for that motor
3. Inject current / short a sense pin to verify I²t, stall, SENSOR_FAULT, and NO_CURRENT trips
4. DC: apply voltage downstream of a closed relay; confirm live V and UV/OV trips (0 = disabled)
5. Confirm missing-SD path (log page banner) and present-SD CSV write
6. Power: DC known load vs bench meter; AC hand-check `V_rated × I_rms`; 3-phase current-unbalance check; trip CSV carries `power_W` / `power_VA`; energy reads `0.00` after reboot
7. Local panel: confirm SH1106 at 0x3C on `Wire` (14/42 pins), encoder detents/tones (CLK 47 / DT 46 / SW 3), GPIO 48 WS2812 status LED, the shared Start gate, and the staged splash

## Changelog

- 2026-09-23 — Jam-release sensor suppression: all sensor-fault checks are skipped while `jam_phase != JAM_IDLE`, so a pulsed/stalled motor cannot trip `SENSOR_FAULT` prematurely. The jam state resets the stale debounce counter before the next recovery attempt, and `processJamRelease` still decides success or exhaustion.
- 2026-09-22 — Stall suppression and startup inrush protection: current-path `SENSOR_FAULT` is immediate and checked before stall; voltage-path `SENSOR_FAULT` needs 3 consecutive faults within 1 s; `start_current` + `icd_ms` override the effective stall threshold during the startup window. Export CSV also falls back to the RAM ring when no SD card is mounted, and jam recovery is capped at 4 attempts.
- 2026-09-22 — I²C contention and swapped bench wiring review: ADS1115 logical map is CH0–3→0x49 and CH4–7→0x48; voltage reads use 128 SPS with mutex-free conversion waits and a 35 ms deadline; protection readiness and boot/docs diagnostics updated.
- 2026-09-22 — Added project-local voltage diagnostics (four raw ADS samples, logical channel/address/AIN, calibration values, validity, and timeout counters) on `SENSOR_FAULT`; no external libraries modified.

- 2026-09-20 — Local OLED + encoder panel: `ui.cpp`/`ui.h`/`ui_icons.h` `uiTask` on core 0, SH1106 on shared ADS1115 `Wire` (GPIO 14/42), KY-040 CLK 47 / DT 46 / SW 3, onboard WS2812 on GPIO 48, boot splash + Home/Per-motor/Fault Log/Diagnostics/Firmware/Network screens, `toneBack()`, shared `protectionCanStart()` gate, non-destructive `protectionCopyLog()` ring, cross-core buzzer mutex, U8g2 + NeoPixel.
- 2026-09-21 — Pin/bus lock: GPIO 22–25 do not exist on ESP32-S3. Encoder is CLK 47 / DT 46 / SW 3. OLED shares the ADS1115 I²C bus (GPIO 14/42) via `U8G2_SH1106_128X64_NONAME_F_HW_I2C`; `s_i2c_mu` serializes every Wire transaction. No `OLED_SDA_PIN`/`OLED_SCL_PIN`/`Wire1`.
- 2026-09-21 — GPIO 48 is the onboard WS2812 (not encoder DT). NeoPixel status LED: worst-state Fault > Running > Cooling > Stopped; thermal gradient while Running; ~2 Hz fault/cooling flash. 3-frame RUNNING/FAULT/COOLING icons (~450 ms). Centered splash.
- 2026-09-21 — ENC_SW is GPIO 3 (strapping-safe JTAG source; KY-040 pull-up holds HIGH at boot). GPIO 19 is USB D- and must never be used as GPIO. Early `Serial.println("MPS-505 boot...")` in `setup()`.
- 2026-09-21 — Boot crash `xQueueSemaphoreTake` on NULL mutex: `uiBegin()` started `uiTask` before `protectionBegin()` created `s_mu`. Mutex/queue create now runs at the top of `setup()`; `init: …` Serial lines between each begin.
- 2026-09-21 — Optional per-motor stall recovery (jam release): 4× 300/500 ms pulses after 500 ms of Running; I²t stays live; stall / UV / OV / NO_CURRENT / SENSOR_FAULT skipped while `jam_phase != IDLE`. 32-entry RAM fault ring feeds the web log when SD is missing (`ram_only`). Live NVS schema is 4.
- 2026-09-21 — Clear Logs clears SD and RAM ring (`protectionClearLog`); OLED rebuilds motor order every 160 ms refresh; web log already served RAM with `ram_only` when SD missing.
- 2026-09-21 — DC voltage filter against false UV/OV: 4-sample ADS average, 0.9/0.1 LPF, `UV_GRACE_MS` 750, `VOLT:` Serial. Relays OUTPUT HIGH at the top of `setup()` before Serial (active-LOW default).
- 2026-09-22 — User-selectable current channels (`ch0`–`ch2`, Auto = lowest free) and per-motor DC voltage sense channel (`vch` / `voltage_channel`, `VCH_SAME` = same as current CH0). NVS schema 4.
- 2026-09-22 — Default relay polarity reversed to active-LOW (`RELAY_ACTIVE_HIGH_DEFAULT = 0`). Boot fail-safe drives GPIO HIGH (OFF). Add/Edit default is Active-LOW.

- 2026-09-22 — Final web log behavior: Export CSV remains visible without an SD card and downloads either the SD file or the newest-first 32-entry RAM ring as `faults.csv`. Stall recovery limit is now 4 pulses.

Last firmware: I²C `i2cInitOnce` / `voltageReprobe` / no `Wire.end()` (origin `455debf` and follow-up quality), on top of Calibrate re-probe (`2f53bc7`), divider 150 kΩ / 10 kΩ (`b5e3754`), and safety review (`912a982`). Current review: 128 SPS conversion scheduling, 35 ms timeout margin, and swapped logical ADS channel map.
