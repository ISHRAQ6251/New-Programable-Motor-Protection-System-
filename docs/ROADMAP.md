# Roadmap — local display and encoder (not this build)

Future enhancement so an operator can run the protector without a laptop. Do not implement until asked.

## Purpose

The web dashboard stays the full configuration UI. The OLED + encoder add **local status and basic control** on the enclosure: which motor is selected, live RMS / DC V / thermal %, last fault, SoftAP IP, Start / Stop / Reset / Calibrate.

Both paths share the same `StatusSnapshot` and command queue. Encoder actions are equivalent to dashboard buttons. Motor add/edit/security stay web-only.

## OLED — SSD1306 128×64 I²C

| Item | Value |
|---|---|
| Part | SSD1306 128×64, 0.96 in, 3.3 V |
| Bus | Existing I²C (GPIO 14 SDA / 42 SCL) |
| Address | **0x3C** (typical; confirm with `I2C: scan`) |
| Library | Adafruit SSD1306 + Adafruit GFX (not added yet) |

Do not call `Wire.begin()` from the display driver. Reuse `i2cInitOnce()` / the already-open `Wire` object. Never `Wire.end()`.

Suggested pages (rotate with the encoder):

1. Overview — motor name, status badge, RMS, thermal %
2. Electrical — live DC V (or rated AC V), power `W`/`VA`, session energy
3. Fault — last type, fault count, cooling remaining
4. Network — SSID `MPS-505`, IP `192.168.4.1`, heap, ADS ok

Refresh from `protectionSnapshot()` at ~2 Hz in a low-priority task on core 0. Do not sample ADC/ADS from the display task.

If 0x3C is missing, Serial `I2C: scan` will omit it; firmware should keep running without the panel.

## Rotary encoder — KY-040

| Item | Value |
|---|---|
| Part | KY-040 (CLK / DT / SW) |
| Pins | two GPIOs + one switch GPIO, **not** 0/3/45/46, 19/20, 43/44, and not the I-sense / relay / SD / I²C set |
| Interrupts | CLK edge on CHANGE; SW polled or FALLING with debounce |

Gestures:

| Gesture | Action |
|---|---|
| Rotate | Next / previous motor (or scroll parameter page) |
| Short press | Start if Stopped; Stop if Running |
| Long press (~2 s) | Reset Fault / Cooling to Stopped |
| Double-click | Calibrate zeros (only when all motors idle) |

Post `CMD_START` / `CMD_STOP` / `CMD_RESET` / `CMD_CALIBRATE` on the existing command queue. Same rules as the web API (DC Start needs ADS + vcal; Calibrate needs all idle).

Handle the encoder in a lightweight task or `loop()`; do not block the protection task.

## Integration rules

- Display and encoder **coexist** with the dashboard; neither is exclusive.
- Protection remains the only writer of relays / I²t / trips.
- I²C: SSD1306 is a third device on the same bus as 0x48 / 0x49. Keep 4.7 kΩ–10 kΩ pull-ups. Scan at boot should then list 0x3C plus the ADS chips.
- No NVS schema bump for display settings unless a later spec says so.
- Do not steal GPIO 14/42 or current-sense 1,2,4,5,6,7,8,9.

## Out of scope until asked

- Graphics beyond 128×64 text/icons
- Editing I²t steps from the encoder
- STA / phone app
- Touch screens
