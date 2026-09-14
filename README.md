# MPS-505 Programmable Motor Protection

University firmware that replaces a bimetallic thermal overload relay with an **ESP32-S3** I²t motor protector.

The MCU measures current on up to eight ACS712-30A sensors, accumulates winding energy as I²t, and opens relays before the motor overheats. A desktop web dashboard is served from a fixed SoftAP. Motor settings live in on-chip NVS. An SD card is optional and used only for a fault-history CSV.

This repository is **firmware and documentation only**. Hardware is assumed already wired.

Full wiring, Arduino IDE steps, dashboard use, and trip math: `docs/USER_MANUAL.md`.

## Features

- Up to **8 channels** = 8 current sensors + 8 relays + 8 DC voltage taps (2× ADS1115)
- **1-phase** motor = 1 channel; **3-phase** motor = 3 linked channels, one dashboard row
- Classic **I²t energy** trip curve (not inverse-time interpolation)
- Independent **stall** trip on the next RMS window
- **Sensor-fault** trip (stuck ADC, out-of-range Vadc, |I| > 40 A, or missing ADS1115 on a DC channel)
- Live **DC voltage** with optional undervoltage / overvoltage trip (0 = that trip disabled)
- **Rated AC voltage** is a manual nameplate field (not sensed, not used for trips)
- Desktop dashboard on SoftAP `MPS-505` / `mps50005`
- Themed `/login` page + HttpOnly session cookie (default `mps` / `mps500`)
- Motor config in NVS; protection still runs if the SD card is missing
- Dual FreeRTOS task split: protection never blocks on SD or HTTP
- Relays fail-safe OFF in `setup()` before Wi-Fi starts

## Hardware

| Item | Spec |
|---|---|
| Board | ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Arduino IDE board | ESP32S3 Dev Module, Flash 16 MB, PSRAM OPI |
| Sensors | ACS712-30A, 66 mV/A, 5 V supply |
| Analog path | 10 k / 15 k divider (x0.6) to ADC1 — about 39.6 mV/A |
| DC voltage | 2× ADS1115 (I²C 0x48 / 0x49), 180 k / 10 k divider, tap downstream of each relay |
| Relays | Logic-level modules, default active-HIGH, boot LOW = OFF |
| SD | 3.3 V SPI breakout (not SDIO, not 5 V) |
| Buzzer | Passive, LEDC PWM |

Zero-current and DC zero-voltage are **calibrated** at boot (relays open → 0 V at each voltage tap). Do not assume 1.5 V or 0.000 V.

Avoid GPIO 0/3/45/46 (strapping), 19/20 (USB-JTAG), 43/44 (UART0 Serial). Current-sense pins are ADC1 only.

### Pin map

All GPIO numbers live only in `MotorProtection/config_pins.h`.

| Function | GPIO |
|---|---|
| I-sense CH0–CH7 | 1, 2, 4, 5, 6, 7, 8, 9 |
| Relay CH0–CH7 | 15, 16, 17, 18, 38, 39, 40, 41 |
| SD MOSI / MISO / SCK / CS | 11 / 13 / 12 / 10 |
| Buzzer | 21 |
| I²C SDA / SCL | 14 / 42 |

## Protection logic (short)

Sampling: at least 32 samples over at least one AC cycle (20 ms at 50 Hz, 16.67 ms at 60 Hz). True RMS. DC uses a 20 ms mean of |i|.

While Running and `I_rms >= k_min × In`:

```
E      += I_rms² × dt          (A²s)
E_trip  = (k × In)² × t_trip
```

Active step = highest `k` with `I_rms >= k × In`. Trip **I2T** when `E >= E_trip`. Below pickup, `E` decays toward 0 over the motor cooling time.

Stall: `I_rms >= stall_amps` on the next window → trip **STALL** immediately.

Sensor fault → trip **SENSOR_FAULT** immediately. Auto-restart still applies.

DC only, after 250 ms of Running: live V below UV (if UV > 0) → **UNDERVOLT**; above OV (if OV > 0) → **OVERVOLT**. Same Cooling / auto-restart / Reset as I²t. AC rated voltage is never compared.

On trip: de-energize that motor's relay(s), sound the fault tone, append an SD log line if a card is mounted, enter **Cooling**. After cooling: auto-restart if enabled, else **Fault**. Reset from the UI returns Fault/Cooling to Stopped and silences the buzzer.

3-phase: one motor, three channels; any phase trips all three relays; the UI shows one row. Thermal % is the hottest phase (`100 × E / E_trip`).

## Flash with Arduino IDE

1. Install **esp32 by Espressif Systems** (Arduino-ESP32 **3.x**) from Boards Manager.
2. Board: **ESP32S3 Dev Module**. Flash **16MB (128Mb)**. PSRAM **OPI PSRAM**. Core Debug Level **Debug**.
3. Library Manager: **ESPAsyncWebServer** and **AsyncTCP** (ESP32Async forks), **Adafruit ADS1X15** and **Adafruit BusIO**.
4. Open the sketch folder `MotorProtection/` and upload.

Built-in (do not install separately): WiFi, Preferences, SD, SPI, LEDC.

Arduino-ESP32 3.x LEDC API used here:

```
ledcAttach(pin, freq, res)
ledcWrite(pin, duty)
ledcChangeFrequency(PIN_BUZZER, freq, 10)
```

The 2-argument `ledcChangeFrequency` does not compile on 3.x.

Serial monitor: **115200** baud. First boot `NVS: no motor blob — starting empty` is expected. Missing SD prints `SD: mount failed — fault log unavailable` and is not a boot failure.

## Connect and sign in

| | |
|---|---|
| SoftAP SSID / password | `MPS-505` / `mps50005` |
| SoftAP IP | `http://192.168.4.1` |
| Dashboard login | `mps` / `mps500` |
| Session cookie | `mps_sess`, HttpOnly, SameSite=Strict, 8 hours |

There is no internet through this network. That is expected. WPA2 needs an 8-character AP password (`mps50005`, not `mps505`). Dashboard login is **not** the AP password. Change credentials on the Security page after first real use.

Pages: Dashboard, Add motor, Edit, Log, Security. Live status polls about once per second. Dashboard shows live RMS, live DC voltage (em dash for AC), and thermal %.

Statuses: **Stopped**, **Running**, **Fault**, **Cooling**. Delete a motor only from Stopped or Fault. Calibrate current and DC voltage zeros only when every motor is Stopped or Fault.

## Repository layout

```
MotorProtection/          Arduino IDE sketch (firmware)
  MotorProtection.ino     setup() / loop(), fail-safe relay OFF
  config_pins.h           GPIO, mV/A, I²C, voltage-divider constants (only file with pin numbers)
  config_limits.h         MAX_*, AP/auth defaults, NVS keys
  types.h                 MotorRecord, commands, statuses
  relays.cpp              Polarity-aware coil drive
  buzzer.cpp              Non-blocking LEDC tones
  sensing.cpp             Current: calibrate + true RMS / DC mean
  voltage.cpp             DC voltage: 2× ADS1115, GAIN_ONE, calibrate
  protection.cpp          I²t / stall / UV / OV / sensor-fault task (prio 5, core 1)
  motor_store.cpp         NVS blob + login credentials
  net_ap.cpp              SoftAP MPS-505
  sd_log.cpp              Optional /faults.csv
  web.cpp, web_html.h     Session login + dashboard
docs/USER_MANUAL.md       Pins, wiring, libraries, user guide, I²t math
docs/superpowers/specs/   Approved design spec
AGENTS.md                 Project source of truth for contributors
LICENSE                   MIT
```

## Limits

| Item | Value |
|---|---|
| Channels / motors / steps | 8 / 8 / 8 |
| RMS samples | ≥ 32 over ≥ 1 AC cycle |
| Sensor \|I\| cap | 40 A |
| ADC | 12-bit, 11 dB, ADC1 only |
| NVS namespace | `mps` (motors blob, auth_user, auth_pass) |
| Fault log | `/faults.csv` — `uptime_ms,motor,type,current_A,voltage_V` |
| NVS schema | 2 (v1 motor blob is discarded on first boot of this build) |
| Timestamps | `millis()` uptime (no NTP on SoftAP) |

## Safety

This is a university protection prototype. Commissioning on a real motor still needs a fused supply, relays rated for the load, and a mechanical emergency stop independent of the ESP32.

Relays default OFF at boot. Confirm polarity before the first Start (default active-HIGH; invert per motor in Add/Edit if the module is active-LOW). SoftAP is for the bench, not as a WAN gateway.

## License

MIT. See `LICENSE`.
