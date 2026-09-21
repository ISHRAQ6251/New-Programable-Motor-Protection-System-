# MPS-505 Programmable Motor Protection — User Manual

Firmware for an ESP32-S3 that replaces a bimetallic thermal overload relay. It measures motor current, runs an I²t thermal model, and trips relays before the motor overheats. Configuration and live status are served on a desktop web dashboard hosted by the ESP32 SoftAP.

This document covers hardware wiring, Arduino IDE setup, libraries, day-to-day use, and a plain-language explanation of the firmware and the trip math.

---

## 1. What the system does

- Up to **8 protection channels**. Each channel is one ACS712-30A current sensor, one relay, and one DC voltage tap (ADS1115).
- A **single-phase** motor uses 1 channel. A **three-phase** motor uses 3 linked channels: a fault on any phase opens all three relays, and the dashboard shows one row, not three.
- Classic **I²t energy** trip curve, plus a faster independent **stall** trip, optional **stall recovery** (jam release), and a **sensor-fault** trip.
- Motor settings live in on-chip flash (NVS). They survive power cycles even if the SD card is missing.
- The SD card is used **only** for a persistent fault-history CSV. Missing card: protection and dashboard still run; the Log page shows the last 32 trips from RAM with a “RAM buffer only — logs lost on reboot” banner.
- Access is local Wi-Fi only: the ESP32 creates network `MPS-505`. There is no router join, no cloud, no DDNS.
- A local **SH1106 OLED + KY-040 encoder** panel shows status and allows Start/Stop at the enclosure. The onboard WS2812 (GPIO 48) is a status LED. The dashboard stays the full configuration UI (motor add/edit, security).

---

## 2. Hardware

### 2.1 Board and power

| Item | Spec |
|---|---|
| MCU | ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Arduino IDE board | ESP32S3 Dev Module |
| Flash | 16 MB (128 Mb) |
| PSRAM | OPI PSRAM |
| ACS712 modules | ACS712-30A, 5 V supply, 66 mV/A |
| Relays | Logic-level modules, default active-HIGH (GPIO HIGH = coil on) |
| SD | RoboticsBD 3.3 V Micro SD breakout, SPI, no onboard regulator |
| Buzzer | 1x passive (PWM) |
| DC voltage | 2× ADS1115 (I²C), 150 k / 10 k divider per channel |
| Local panel | SH1106 128x64 OLED (I²C 0x3C) + KY-040 encoder + onboard WS2812 (GPIO 48) |

Power the ESP32 from USB or a 5 V supply that shares ground with the ACS712 boards and relay modules. The SD breakout is **3.3 V only** — do not feed it 5 V.

### 2.2 Pins to avoid on ESP32-S3

Do not use these GPIOs for sensors or relays:

| GPIO | Why |
|---|---|
| 22–25 | Do not exist on this chip at all (not physical pins) |
| 26–37 | Reserved for flash / octal PSRAM on this N16R8 module |
| 0, 3, 45 | Strapping pins (boot-mode / flash / JTAG) — fully off-limits |
| 46 | Input-only strapping pin (ROM extra boot-log text only, unrelated to boot/flash/JTAG). Used here as ENC_B (DT) only. Distinct from 0/3/45. |
| 20 | Native USB D+ on silicon; unused on this board (programming/Serial go through the UART bridge) |
| 43, 44 | UART0 Serial monitor — do not reassign |

Current-sense pins are all **ADC1**. ADC2 is unreliable while Wi-Fi is on.

### 2.3 Complete pin map

All GPIO numbers exist only in `MotorProtection/config_pins.h`. Changing a pin means editing that file and reflashing.

| Function | GPIO | Direction | Notes |
|---|---|---|---|
| I-sense CH0 | 1 | Analog in (ADC1) | ACS712 via divider |
| I-sense CH1 | 2 | Analog in (ADC1) | |
| I-sense CH2 | 4 | Analog in (ADC1) | GPIO 3 skipped (strapping) |
| I-sense CH3 | 5 | Analog in (ADC1) | |
| I-sense CH4 | 6 | Analog in (ADC1) | |
| I-sense CH5 | 7 | Analog in (ADC1) | |
| I-sense CH6 | 8 | Analog in (ADC1) | |
| I-sense CH7 | 9 | Analog in (ADC1) | |
| Relay CH0 | 15 | Digital out | Active-HIGH default, boot LOW = OFF |
| Relay CH1 | 16 | Digital out | |
| Relay CH2 | 17 | Digital out | |
| Relay CH3 | 18 | Digital out | |
| Relay CH4 | 38 | Digital out | |
| Relay CH5 | 39 | Digital out | |
| Relay CH6 | 40 | Digital out | |
| Relay CH7 | 41 | Digital out | |
| SD MOSI | 11 | SPI | 3.3 V only |
| SD MISO | 13 | SPI | |
| SD SCK | 12 | SPI | |
| SD CS | 10 | SPI chip select | |
| Buzzer | 21 | PWM (LEDC) | Passive buzzer |
| I²C SDA | 14 | I²C | Shared ADS1115 + SH1106 — not ESP32 default GPIO 8 |
| I²C SCL | 42 | I²C | Shared ADS1115 + SH1106 — not ESP32 default GPIO 9 |
| Encoder CLK | 47 | Digital in | KY-040 A, plain `INPUT` (module pull-up) |
| Encoder DT | 46 | Digital in | KY-040 B. GPIO 46 is input-only/strapping; encoder line only. |
| Encoder SW | 19 | Digital in | KY-040 switch, active-LOW. Native USB unused on this board; programming/Serial go through UART 43/44. |
| Status LED | 48 | One-wire | Onboard WS2812. Not I²C — never take `s_i2c_mu`. |
| UART0 Serial | 43, 44 | reserved | Do not reassign |

### 2.4 Analog front-end (every current channel)

The ACS712-30A runs on 5 V. Its output sits near 2.5 V at zero current and swings about 0.5–4.5 V, which is too high for the ESP32-S3 ADC. Scale it with a resistive divider (no op-amp):

```
ACS712 Vout -- 10 kOhm --+-- to ESP32 GPIO (ADC)
                         |
                       15 kOhm
                         |
                        GND
```

- Divider ratio = 15 / (10 + 15) = **0.6**
- At the ADC: about **1.5 V** at zero current, **39.6 mV/A** (`66 mV/A × 0.6`)
- Firmware **calibrates** the real zero-current voltage at boot. Do not assume 1.5 V; resistor and sensor tolerances move it.

Current conversion used in firmware:

```
I (A) = (Vadc_mV - zero_mV) / 39.6
```

### 2.5 Wiring diagram (one channel, then scale to 8)

```
                    +5 V                 3.3 V
                     |                     |
               +-----+-----+         +-----+-----+
               | ACS712-30A|         | ESP32-S3  |
  Motor phase  |  IP+  IP- |         |  N16R8    |
  ----(load)---+           |         |           |
               |    VIOUT--+--10k--+ | GPIOx     |   x = 1,2,4,5,6,7,8,9
               |           |       | | (ADC1)    |
               +-----+-----+     15k |           |
                     |             | |           |
                    GND-----------GND|-----------GND
                                       |           |
                                       | GPIOy     |   y = 15,16,17,18,38,39,40,41
                                       |    |      |
                                       |    +-- IN of relay module CH
                                       |           |
                     +5 V -- relay VCC-+           |
                    GND  -- relay GND--+           |
                                        | GPIO 21 -- passive buzzer -- GND
                                        | GPIO 14 -- I2C SDA -- ADS1115 SDA (both chips)
                                        | GPIO 42 -- I2C SCL -- ADS1115 SCL
                                        | GPIO 11 -- SD MOSI
                                        | GPIO 13 -- SD MISO
                                        | GPIO 12 -- SD SCK
                                        | GPIO 10 -- SD CS
                                        | 3.3 V  -- SD VCC / ADS1115 VDD  (NOT 5 V)
                                        +----------- SD GND / ADS GND
```

Relay modules typically need **5 V** for the coil supply and a **3.3 V-tolerant** IN pin. Confirm your module datasheet. Default firmware polarity: IN HIGH = motor power ON. Per-motor polarity can be inverted in the Add/Edit form if the module is active-LOW.

**Fail-safe:** `setup()` drives every relay pin LOW before Wi-Fi starts. With the default active-HIGH wiring, motors stay off across reset.

### 2.6 DC voltage sensing (ADS1115)

ESP32 ADC1 is fully used by current sensing. DC voltage uses two ADS1115 16-bit ADCs on I²C:

| Module | ADDR pin | I²C address | Channels |
|---|---|---|---|
| ADS #1 | GND | 0x48 | CH0–CH3 → AIN0–AIN3 |
| ADS #2 | VDD | 0x49 | CH4–CH7 → AIN0–AIN3 |

Firmware calls `i2cInitOnce()` once (`Wire.begin(14, 42)`). Adafruit BusIO's `ads.begin()` may call `Wire.begin()` with no pins, so the sketch re-binds 14/42 after each `begin()`. **Never `Wire.end()`** — it destroys the driver (see `docs/I2C_TROUBLESHOOTING.md`). Gain is **GAIN_ONE** (±4.096 V). Both ADS1115 modules and the SH1106 OLED share that bus, serialized by `s_i2c_mu`; use 4.7 kΩ–10 kΩ pull-ups on SDA/SCL to 3.3 V if the breakout boards do not already have them. Calibrate re-runs `voltageReprobe()` so an ADDR-pin fix is picked up without a reboot.

Each voltage divider taps the **motor terminal downstream of that channel's relay**, not the shared DC bus:

```
Motor terminal (after relay) -- 150 kOhm --+-- ADS1115 AINx
                                           |
                                         10 kOhm
                                           |
                                          GND
```

- Scale = (150k + 10k) / 10k = **16**
- 0–50 V → about **0–3.13 V** at the ADS input
- Firmware **calibrates** the AIN zero with the relay open (tap sits at 0 V). Do not assume 0.000 V.

Conversion:

```
V_bus (V) = (V_adc - v_zero) × 16
```

There is **no AC voltage sensor**. For AC motors, enter a static **Rated AC voltage** on Add/Edit. It is stored in NVS and shown as configuration, not as live telemetry, and is not used for trips.

### 2.7 Three-phase motors

Wire phases A, B, C to three consecutive free channels (the firmware allocates the lowest free indices, e.g. CH0/CH1/CH2). The motor supply for all three phases should pass through the three relays so that a trip on any phase de-energizes the whole motor.

---

## 3. Arduino IDE — libraries and board settings

### 3.1 Board package

Install **esp32 by Espressif Systems** (Arduino-ESP32 **3.x**, e.g. 3.0–3.3) from Boards Manager.

Board menu:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| USB CDC On Boot | Enabled (optional, convenient Serial) |
| Core Debug Level | Debug |
| Upload Speed | 921600 (or slower if uploads fail) |

Open the sketch folder `MotorProtection/` (the `.ino` plus the `.cpp` / `.h` files in the same folder).

### 3.2 Libraries (Library Manager)

| Library | Author / fork | Required |
|---|---|---|
| **ESPAsyncWebServer** | ESP32Async | Yes |
| **AsyncTCP** | ESP32Async | Yes (dependency of the server) |
| **Adafruit ADS1X15** | Adafruit | Yes (DC voltage) |
| **Adafruit BusIO** | Adafruit | Yes (dependency of ADS1X15) |
| **U8g2** | olikraus | Yes (local OLED panel) |
| **Adafruit NeoPixel** | Adafruit | Yes (onboard WS2812 status LED on GPIO 48) |

Built in with the ESP32 core (do not install separately):

| Library | Used for |
|---|---|
| WiFi | SoftAP |
| Preferences | NVS motor list and login |
| SD, SPI, FS | Optional fault CSV |
| LEDC (esp32-hal-ledc) | Buzzer PWM |
| Wire | I²C for ADS1115 and the SH1106 panel (pins 14 / 42, mutex `s_i2c_mu`) |

If compile fails on Arduino-ESP32 3.x, use the maintained **ESP32Async** forks of both libraries, not the older me-no-dev copies.

### 3.3 First flash checklist

1. USB cable that carries data, not charge-only.
2. Select the correct COM port.
3. Compile and upload `MotorProtection`.
4. Open Serial Monitor at **115200** baud.
5. Confirm the boot banner (see section 4.1).

---

## 4. Connecting and signing in

### 4.1 Boot banner (Serial 115200)

On every reset the firmware prints something like:

```
I2C: initialized SDA=14 SCL=42 clock=400000Hz
I2C: scan 0x48
I2C: expected 0x48 (ADDR->GND)=CH0-3, 0x49 (ADDR->VDD)=CH4-7
ADS: 0x48 ok GAIN_ONE 250SPS
ADS: 0x49 not found (I2C err=2 NACK addr)
NVS: no motor blob — starting empty
SD: mount failed — card missing, 5 V on a 3.3 V breakout, or CS/SPI wiring
SD: fault log unavailable; protection still runs
---- MPS-505 boot ----
mode: SoftAP
SSID: MPS-505
AP pass: mps50005
IP:   192.168.4.1
dash user: mps
dash pass: mps500
----------------------
HEAP: boot=...
```

`SD: mount failed` is normal if no card is inserted. Protection still runs. `ADS: 0x48/0x49 not found` is normal if that chip is unpopulated; DC motors on those channels cannot Start. Dashboard login is **`mps` / `mps500`**, not the AP password. If Serial ever printed `dash pass: mps50005`, reflash this tree — load restores the dashboard default.

### 4.2 Join the access point

On a desktop (Windows / Linux / macOS):

1. Connect Wi-Fi to **MPS-505**, password **mps50005**.
2. There is no internet through this network. That is expected.
3. Open a browser to `http://192.168.4.1` — you are redirected to `/login`.

WPA2 requires an 8-character password. The AP password is `mps50005`, not `mps505`.

If you previously joined a leftover SSID such as `MPS`, forget that network on the PC first.

### 4.3 Dashboard login

Default username **mps**, password **mps500**.

This is a themed HTML page, not the browser's built-in Basic Auth dialog. A successful login sets an HttpOnly cookie `mps_sess` (valid 8 hours). Use **Log out** in the header, or change credentials on the Security page (that invalidates the session and you must sign in again).

---

## 5. Using the dashboard

Desktop layout only (wide tables). Pages:

| Page | Purpose |
|---|---|
| Dashboard | One row per motor: channels, name, status, uptime, fault count, live RMS, live DC V (em dash for AC), power and session energy, thermal %, Start / Stop / Reset |
| Add motor | Wizard: phase count first, then name and protection settings |
| Edit | Change settings or delete (delete only from Stopped or Fault) |
| Log | Fault CSV when SD is mounted. Without SD: 32-entry RAM ring + “RAM buffer only” banner |
| Security | Change dashboard username and password |

Live values poll about once per second.

### 5.1 Status meanings

| Status | Relays | Meaning |
|---|---|---|
| Stopped | OFF | Idle. Start is allowed |
| Running | ON | Sampling current, accumulating I²t |
| Cooling | OFF | Just tripped. Waiting `cooling_s` seconds |
| Fault | OFF | Cooling finished and auto-restart is off, or 3 consecutive trips. Press Reset, then Start |

Reset is enabled in Fault and Cooling. It returns the motor to Stopped and silences the fault buzzer. Stop is only enabled while Running.

### 5.2 Add a motor

1. Choose **1** or **3** phases. The UI shows which channels will be allocated (lowest free indices). If not enough channels are free, add is rejected.
2. **Name** — letters, digits, space, `_ - .` only.
3. **Operating current In (A)** — the motor's rated current. Trip steps are multiples of this.
4. **Supply** — AC or DC.
   - AC: pick **50 Hz** or **60 Hz** (RMS window) and **Rated AC voltage** (nameplate only; not sensed, not a trip).
   - DC: optional **Undervoltage** and **Overvoltage** in volts. **0 disables** that trip. If both are set, OV must be greater than UV. Live V is the terminal downstream of the relay.
5. **Stall current (A)** — instantaneous trip if RMS reaches this on the next sample window, unless stall recovery is enabled. Must exceed In and every I²t step current.
6. **Cooling time (s)** — wait after a trip before auto-restart or Fault. Also used as the I²t decay time while running below pickup.
7. **Auto-restart** — after cooling, return to Running (On) or stay in Fault (Off). On still latches Fault after 3 consecutive trips; a 10-minute trip-free run, Start, or Reset clears the counter.
8. **Stall recovery (jam release)** — optional. Off by default. See §5.2.1.
9. **Relay polarity** — Active-HIGH (default) or Active-LOW for this motor's channels.
10. **N protection steps** (1–8). For each step enter:
   - **k × In** — current multiplier (e.g. 1.2 means 1.2 × operating current)
   - **trip time (s)** — how long that energy budget lasts at exactly `k × In`

Example curve for In = 10 A:

| Step | k | t_trip | Meaning at exactly that current |
|---|---|---|---|
| 1 | 1.05 | 3600 | 10.5 A allowed for 1 hour of equivalent energy |
| 2 | 1.2 | 180 | 12 A allowed for 3 minutes of equivalent energy |
| 3 | 1.5 | 30 | 15 A allowed for 30 s of equivalent energy |
| 4 | 6.0 | 2 | 60 A allowed for 2 s of equivalent energy |

Stall might be set to 45 A so a locked rotor trips on the next ~20 ms window, without waiting for I²t.

### 5.2.1 Stall recovery (jam release)

Optional per-motor checkbox on Add / Edit: **Stall recovery (jam release)**. Default **off**.

When enabled and the motor has been Running for at least **500 ms**, a stall does **not** trip immediately. Instead the firmware pulses the relay(s) to try to free a jam (a stuck conveyor object, not a locked rotor that should trip):

1. De-energize **300 ms**, then re-energize.
2. Wait **500 ms** for spin-up, then re-check current.
3. If current is below stall: resume Running and zero the jam-attempt count.
4. If still stalled: repeat, up to **3** pulses total.
5. If all 3 fail: trip `STALL` and enter Cooling as usual.

Timing is fixed (not user-configurable). Jam attempts are a **separate counter** from auto-restart. A successful jam recovery does **not** clear the auto-restart counter. After a failed jam sequence the motor cools, then auto-restart (if enabled) does a full Start — not more jam pulses — and the 500 ms run-in applies again.

While pulses are in progress, I²t energy still accumulates and `SENSOR_FAULT` still trips. Stall, UV/OV, and `NO_CURRENT` are skipped until the sequence ends (the 500 ms wait is the settle window; there is no extra grace).

**Do not enable** on pumps, precision equipment, or fragile loads. The pulses are short, but they still apply power to a stalled machine.

The dashboard shows `JAM (n/3)` next to status while a sequence is active. The local OLED panel does not show jam state.

### 5.3 Start, stop, reset, calibrate

- **Start** — only from Stopped, and only after zero calibration (done automatically at boot). DC Start also needs the ADS1115 for that motor's channels.
- **Stop** — opens the relay(s), zeros thermal energy, power, session energy, and live readings.
- **Reset** — from Fault or Cooling, same as Stop plus silence buzzer.
- **Calibrate zeros** — re-probes I²C for the two ADS1115 chips, then re-measures current ADC zeros and DC voltage zeros on all 8 channels. Allowed only when every motor is Stopped or Fault. Relays stay off so voltage taps sit at 0 V. A wiring fix to an ADDR pin is picked up here without a reboot.

### 5.4 Fault log (SD + RAM)

Every trip is written to a **32-entry RAM ring** (most recent first). If an SD card is mounted, the same event is also appended to `/faults.csv`.

If a card is mounted: file `/faults.csv` on the card. The Log page shows the full CSV. Export downloads it. Clear rewrites the SD header and also empties the 32-entry RAM ring.

```
uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA
15230,Pump1,I2T,18.400,24.100,446.4,
15231,Fan2,I2T,3.200,,,,312
```

DC rows fill `power_W`; AC rows fill `power_VA`; the other column is blank. Power is the value from the sample window that tripped.

Types: `I2T`, `STALL`, `SENSOR_FAULT`, `UNDERVOLT`, `OVERVOLT`, `NO_CURRENT`. Time is milliseconds since ESP32 boot (no NTP on SoftAP). Older 4- or 5-column rows still parse; missing columns read as blank.

No card: the Log page shows the RAM ring and a yellow/amber banner **RAM buffer only — logs lost on reboot**. Export stays SD-only (hidden). Clear empties the RAM ring (always available). Motors still protect. The OLED Fault Log uses the same RAM ring (also lost on reboot). Fault logs are never written to NVS.

### 5.5 Buzzer patterns

| Event | Sound |
|---|---|
| Power-up | Three ascending beeps |
| Fault | Repeating 1 kHz / silence until Reset or Stop |
| Motor added | Two short beeps |
| Started | Rising chirp |
| Stopped | Falling chirp |
| UI click | 20 ms tick |
| Panel back (long press) | Short two-note descending blip |

### 5.6 Local panel (SH1106 OLED + KY-040 encoder + WS2812)

The panel gives at-a-glance status and Start/Stop without a laptop. It is optional: if the OLED is missing the firmware keeps running and the web dashboard still works. Encoder is CLK 47 / DT 46 / SW 19. GPIO 48 is the onboard WS2812 status LED (one-wire, not I²C).

Controls:

- **Rotate** — move the selection, cycle motors, or scroll.
- **Short press** — on Home, open the highlighted row; on Per-motor, Start/Stop that motor; on the info screens, nothing.
- **Long press (~0.6 s)** — go back to Home.

Screens:

- **Boot splash** — relay fail-safe, ADS1115 probe, SD mount, calibration, then auto-advance to Home.
- **Home** — one row per motor (status icon + name), then Fault Log / Diagnostics / Firmware Info / Network Info.
- **Per-motor** — status, live current, segmented thermal bar, DC volts and power (or rated V / VA).
- **Fault Log** — most recent trips first, with fault type, current, and voltage.
- **Diagnostics** — 0x48/0x49 found or missing (with the I2C error label), per-channel DC calibration, SD mounted, free heap.
- **Firmware Info** — build date/time and uptime.
- **Network Info** — SoftAP SSID/password, AP IP, and the dashboard login.

Status is shown as an icon: square = Stopped (static), chevron = Running, warning triangle = Fault, snowflake = Cooling. RUNNING / FAULT / COOLING cycle three frames about every 450 ms. A fault also shows a short word (`I2T`, `STALL`, `SENSOR`, `UNDER-V`, `OVER-V`, `NO-I`).

The onboard LED follows the worst motor: Fault > Running > Cooling > Stopped (off if none or all Stopped). Fault colour is STALL red, SENSOR magenta, NO_CURRENT cyan, OVERVOLT white, UNDERVOLT yellow, I2T orange, flashing at about 2 Hz. Running uses a blue→cyan→green→yellow→orange thermal gradient from the hottest motor. Cooling flashes blue.

A Start is refused with a short on-screen reason and a fault beep when a DC motor's ADS1115 or voltage calibration is not ready — the same rule the dashboard enforces.

The panel shares the ADS1115 I²C bus (GPIO 14/42); every Wire transaction is held under `s_i2c_mu` only for the duration of that call. If the display stays blank, check the address (0x3C) and that U8g2 is using the primary `HW_I2C` constructor.

---

## 6. Code map (what each file does)

Sketch folder: `MotorProtection/`. Arduino IDE compiles every `.ino` / `.cpp` in that folder.

```
MotorProtection.ino     setup() / loop(), fail-safe relay OFF, spawn protection task
config_pins.h           ONLY file with GPIO numbers, mV/A, I²C, voltage divider
config_limits.h         MAX_CHANNELS=8, steps, NVS keys, AP and login defaults
types.h                 MotorRecord, commands, statuses, snapshots
relays.cpp              Polarity-aware coil drive
buzzer.cpp              Non-blocking LEDC tone sequencer
sensing.cpp             Current ADC calibrate, true RMS (AC) or mean |i| (DC)
voltage.cpp / voltage.h 2× ADS1115, i2cInitOnce, voltageReprobe, GAIN_ONE
protection.cpp          I²t / stall / jam release / UV / OV / sensor-fault / NO_CURRENT / power
motor_store.cpp         NVS blob + login credentials
net_ap.cpp              SoftAP MPS-505 / mps50005
sd_log.cpp              Optional /faults.csv
ui.cpp / ui.h           Local panel: uiTask, OLED + encoder + WS2812 LED, boot splash + screens
ui_icons.h              8x8 XBM status (3-frame RUNNING/FAULT/COOLING) and AP icons
web.cpp / web_html.h    Async HTTP, session login, dashboard HTML
```

Boot order in `setup()` (order matters for fail-safe):

1. Relays: all pins OUTPUT LOW
2. Buzzer, local panel (`uiBegin()` starts `uiTask` and the boot splash), current ADC, ADS1115 (`i2cInitOnce()` → GPIO 14/42)
3. NVS load (empty list if missing/corrupt/schema mismatch — never invents motors)
4. De-energize relays using stored polarities; publish boot stages to the splash
5. Try SD mount (failure is a warning only)
6. Calibrate current and DC voltage zeros, start protection task (priority 5, core 1)
7. SoftAP + web server
8. Print banner, finish the splash, play power-up tone

Three tasks after boot:

- **Protection task** — sample current ADC and ADS1115 **outside** the status mutex, then apply I²t / stall / UV / OV / sensor-fault. Never writes SD or HTTP.
- **`uiTask`** — core 0: poll the encoder, render the OLED, drive the GPIO 48 WS2812, and post Start/Stop through the shared gate. Reads state only through the snapshot.
- **`loop()`** — play queued buzzer tones, append SD log lines, keep the async web server running.

Web handlers and the local panel enqueue `START` / `STOP` / `RESET` / `CALIBRATE` / `RELOAD`. Both read a mutex-guarded snapshot (RMS, DC volts, status, thermal %, heap, SD flag, ADS ok) and share `protectionCanStart()` before a Start.

---

## 7. The math, without the jargon

### 7.1 Why I²t?

Copper windings heat roughly with **current squared**. Energy in the thermal model is:

```
E  +=  I_rms²  ×  dt     (units: ampere² · seconds)
```

A protection **step** `(k, t_trip)` says: "at current `k × In`, trip after `t_trip` seconds." The energy budget for that step is:

```
E_trip  =  (k × In)²  ×  t_trip
```

The motor trips I2T when accumulated `E` reaches `E_trip` of the **active** step. The active step is the highest `k` such that measured current is at least `k × In`.

Worked numbers: In = 10 A, one step k = 1.5, t_trip = 30 s.

- At a steady 15 A: E_trip = 15² × 30 = 6750 A²s. Time to trip ≈ 30 s.
- At a steady 30 A: energy piles up four times faster (30² / 15² = 4), so trip in about 7.5 s.
- At 12 A, if the next lower step is k = 1.2, t = 180 s: E_trip = 12² × 180 = 25920 A²s.

Dashboard **thermal %** is `100 × E / E_trip` of the active step, using the hottest phase on a 3-phase motor. Below the lowest k (pickup), % is 0 and E **decays** toward 0 over the motor's cooling time, like a winding cooling under light load:

```
E  :=  E × (1 − dt / cooling_s)
```

E is also zeroed on Stop, Reset, and when leaving Cooling.

This is **classic I²t energy**, not inverse-time interpolation and not independent per-step timers.

### 7.2 RMS vs DC

AC current is a sine; a single ADC sample can be zero at the wrong instant. The firmware takes **at least 32 samples** spanning **at least one full cycle**:

- 50 Hz → 20 ms window
- 60 Hz → 16.67 ms window

True RMS:

```
I_rms  =  sqrt( mean( i² ) )
```

DC motors skip RMS and use the mean of |i| over 20 ms.

### 7.3 Stall

If `I_rms >= stall_amps` after **one** sample window, trip type `STALL` immediately — unless stall recovery is enabled for that motor (see §5.2.1). No I²t wait on the stall path. Typical use without recovery: locked rotor, many times In.

With stall recovery: after 500 ms of Running, up to 3 pulses (300 ms off, 500 ms wait). I²t still accumulates during pulses. Exhaustion trips `STALL` and enters Cooling. Jam attempts do not count as auto-restarts.

### 7.4 Sensor fault

Immediate trip `SENSOR_FAULT` if, on an assigned channel while Running:

- Mean ADC voltage outside **0.05–3.05 V** (open or shorted divider)
- |I_rms| above **40 A** (beyond a 30 A sensor)
- AC or DC reading **stuck** (identical millivolt samples across a full window)
- DC: ADS1115 missing / I²C timeout, |V_adc| > 4 V, or |V_bus| > 55 V

Same path as other trips: relays off, fault tone, SD log if present, Cooling. Auto-restart still applies (this is trip-immediately, not latched).

If every phase of a Running motor stays below **0.05 × In** for **2 s**, trip `NO_CURRENT`. That catches a broken sense wire sitting at ACS712 mid-rail (~0 A), an open winding, or a relay that never closed.

### 7.5 DC undervoltage / overvoltage

After 250 ms of Running (so the relay has closed and the tap is live):

- If UV > 0 and `V_bus < UV` → trip `UNDERVOLT`
- If OV > 0 and `V_bus > OV` → trip `OVERVOLT`

0 disables that trip. AC rated voltage is never compared.

### 7.6 Power display and its limits

Every motor row shows power and session energy, computed from the current and voltage samples the protection task already takes. No extra sensor or sampling pass is added.

| Supply | What is measured | What the row shows | Can we call it watts? |
|---|---|---|---|
| DC | Voltage (ADS1115) and current both measured | True power `P = V × I` in `W` | Yes — real watts |
| AC 1-phase | Current measured; voltage is the nameplate number | Apparent power `~ S = V_rated × I_rms` in `VA` | No — VA, not W |
| AC 3-phase | Current on all three phases; voltage is the nameplate line-to-line number | Apparent power `~ S = (V_rated / √3) × (I_a + I_b + I_c)` in `VA` | No — VA, not W |

Key points:

- **Power factor is unknown.** This hardware cannot measure the phase angle between voltage and current, so an AC reading is apparent power in volt-amperes, never true watts. Do not read `VA` as `W`.
- **3-phase uses the sum of currents**, which equals `√3 × V_LL × I_avg` when the motor is balanced. If one phase is high, the sum still reflects total current rather than only the worst phase. Voltage is still assumed balanced.
- **Energy** (`Wh` for DC, `VAh` for AC) accumulates only while the motor is Running, at roughly one sample per second. It shows `0.00` in Stopped, Fault, and Cooling, and resets on every Start and on reboot. Treat the label "since boot" as "since the last Start".
- Values come from the same window that trips the motor, so the fault CSV records the power at the moment of the trip.

### 7.7 State machine

```
Stopped --Start--> Running
Running --Stop---> Stopped
Running --stall, recovery On, after 500 ms run--> jam pulses (3× 300/500 ms)
jam recovered --> Running (jam_count=0; auto-restart counter unchanged)
jam exhausted --> Cooling as STALL
Running --trip---> Cooling      relays OFF, log, fault tone
Cooling --timer + auto-restart On and consecutive trips < 3 --> Running
Cooling --timer + auto-restart Off or 3 consecutive trips--> Fault
Cooling or Fault --Reset--> Stopped
```

Start is rejected from Fault/Cooling, if zeros were never calibrated, or (DC) if the ADS1115 for that motor's channels is missing.

---

## 8. Defaults and limits (quick reference)

| Item | Value |
|---|---|
| SoftAP SSID / password | `MPS-505` / `mps50005` |
| SoftAP IP | 192.168.4.1 |
| Dashboard login | `mps` / `mps500` (change on Security) |
| Session cookie | `mps_sess`, HttpOnly, 8 hours |
| Channels / motors / steps | 8 / 8 / 8 |
| ACS712 | 30 A, 66 mV/A, divider ×0.6 → 39.6 mV/A |
| DC voltage | ADS1115 GAIN_ONE, 150 k / 10 k (×16), I²C 14/42 |
| ADC | 12-bit, 11 dB attenuation, ADC1 only (current) |
| RMS samples | ≥ 32 over ≥ 1 AC cycle |
| Sensor |I| cap | 40 A |
| UV/OV grace | 250 ms after DC Start |
| No-current trip | Running and max phase `< 0.05 × In` for 2 s |
| Auto-restart cap | 3 consecutive trips, then Fault; 10 min clean run clears |
| Stall recovery | 3 pulses, 300 ms off / 500 ms wait, 500 ms min run; default off |
| Task WDT | 5 s on the protection task (hang → MCU reset, relays off) |
| Fault log | SD `/faults.csv` when present; else 32-entry RAM ring (`ram_only`) |
| Power | DC `W` (true); AC `VA` (apparent at rated V); PF unknown |
| Session energy | RAM only, `Wh`/`VAh`, resets on Start and reboot |
| NVS namespace | `mps` (motors blob, auth_user, auth_pass), schema 3 |

---

## 9. Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| No Wi-Fi named MPS-505 | Short AP password leftover, or old SSID cached | Flash current firmware (`mps50005` is 8 chars). Forget old `MPS` network. Read Serial banner |
| Browser asks for Basic Auth | Old firmware | Reflash this tree; login is `/login` |
| Login page rejects mps / mps500 | Credentials changed in NVS | Serial banner prints the current pair. Or erase flash / NVS |
| Relays chatter or motors run at boot | Active-LOW module with default HIGH polarity | Set polarity to Active-LOW on Add/Edit, or invert the IN wiring |
| Thermal % stuck / false SENSOR_FAULT with no motor | Uncalibrated zero or floating sense pin | Calibrate with no current. Ground unused sense inputs through the divider |
| Log page yellow banner “RAM buffer only” | No SD or 5 V fed to a 3.3 V breakout | Insert a FAT-formatted card on 3.3 V SPI for persistent CSV; RAM ring still shows last 32 trips |
| Compile error `ledcChangeFrequency` | Mixing ESP32 core 2.x vs 3.x | Use Arduino-ESP32 3.x and `ledcAttach` / 3-arg `ledcChangeFrequency` |
| Heap numbers falling forever | Leak (should stabilize ~200 kB free after login) | Capture Serial heap lines; expected small sawtooth from TCP |
| DC Start disabled / "needs ADS1115" | Chip unpopulated, ADDR pin wrong, SDA/SCL swap, or I²C on GPIO 8/9 | Serial `I2C: scan` then `ADS: 0x48/0x49`. CH0–3 always 0x48, CH4–7 always 0x49. Press Calibrate (`voltageReprobe`); see `docs/I2C_TROUBLESHOOTING.md` |
| `I2C: diag ads_ok=[0,…]` every 5 s | Expected chip missing or bus fault | Match the scan list to 0x48/0x49. `err=2` (NACK addr) = nothing at that address; `err=5` (timeout) = stuck bus / SDA-SCL swap / missing pull-ups |
| OLED blank / no local panel | U8g2 not installed, wrong address, or bus contention | Confirm 0x3C on the shared Wire bus (GPIO 14/42). Constructor is `U8G2_SH1106_128X64_NONAME_F_HW_I2C` |
| Encoder direction reversed | CLK/DT swapped for your module | Swap the CLK and DT wires (CLK 47 / DT 46 / SW 19). Bounce is already absorbed by the state-table decoder |
| Status LED dark or wrong colour | Adafruit NeoPixel not installed, or GPIO 48 used as encoder DT | Install NeoPixel. GPIO 48 is the WS2812 only — encoder DT is GPIO 46 |
| Panel shows `DC start needs the ADS1115…` | Same DC-readiness gate as the dashboard | Populate/repair the ADS1115, then press Calibrate |
| Login page rejects mps / mps500 after a flash that printed `dash pass: mps50005` | NVS `auth_pass` was the AP password | This build restores `mps500` when `auth_pass` equals the AP password |
| Live DC V stuck at 0 with motor running | Tap is upstream of the relay, or not calibrated | Tap **downstream** of the relay. Calibrate with motors Stopped |
| Live DC V reads ~19 % high vs a meter | Firmware still using the old 180 k / 10 k scale | Flash this tree (150 k / 10 k, scale 16) and recalibrate zeros |
| Edit rejected "stall current must exceed In…" | Stall ≤ In or ≤ a protection-step current | Raise stall above In and every `k × In` |
| Motor trips `NO_CURRENT` at Start | Sense wire open, ACS712 unpowered, or relay never closed | Check ACS712 5 V, sense wiring, and that the relay actually closes |
| Motors vanished after this flash | NVS schema 3 vs v1/v2 blob | Expected. Re-enter motors. Auth credentials are unchanged |

---

## 10. Safety notes

- This firmware is a university protection prototype. Commissioning on a real motor still needs a fused supply, correct relay rating, and a mechanical emergency stop independent of the ESP32.
- Relays default OFF at boot. Confirm polarity before the first Start.
- SoftAP has no internet isolation from a production network — use it on the bench, not as a WAN gateway.
- Change the dashboard password on first real use (Security page).
