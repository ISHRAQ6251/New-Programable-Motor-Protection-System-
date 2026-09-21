# Roadmap

Ideas not yet built. This file no longer describes the local OLED/encoder panel —
that shipped on 2026-09-20 and its design lives in
`docs/superpowers/specs/2026-09-20-oled-encoder-local-panel-design.md`.

## Local panel (built 2026-09-20)

- SH1106 128×64 OLED on a dedicated second I²C bus, `Wire1` (GPIO 25 SDA / 47 SCL)
- KY-040 encoder on GPIO 22 (CLK) / 23 (DT) / 24 (SW)
- `uiTask` on core 0: boot splash, Home, Per-motor, Fault Log, Diagnostics,
  Firmware Info, Network Info
- Start/Stop shares the web dashboard's exact gate via `protectionCanStart()`
- Buzzer feedback: `toneClick()` per detent, `toneBack()` on long-press, and the
  protection tone on Start/Stop

Out of scope for the panel, unchanged:

- Motor add / edit / delete and security settings stay web-only
- Panel does not edit I²t steps
- Panel is status + basic control only; protection remains the only writer of
  relays, I²t, and trips

## Still open

- The `_2ND_HW_I2C` constructor takes only `(rotation, reset)`; SDA/SCL are set
  on `Wire1` before `u8g2.begin()` and re-bound right after. If `Wire1` is ever
  found genuinely unavailable on a target, only then consider the bit-banged
  `_SW_I2C` constructor — do not use it to dodge the constructor signature.
- Consider a `SD`/`heap` line on Home for at-a-glance health (currently on
  Diagnostics).

## Out of scope until asked

- Graphics beyond 128×64 text/icons
- Editing I²t steps from the encoder
- STA mode / phone app
- Touch screens
- NVS persistence of panel settings (no schema bump planned)
