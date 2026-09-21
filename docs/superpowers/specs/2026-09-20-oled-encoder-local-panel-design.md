# Local OLED + encoder panel — design

Date: 2026-09-20
Status: implemented (source only; not flashed)

## Purpose

Give an operator local status and basic Start/Stop control on the enclosure,
without a laptop. The web dashboard remains the full configuration UI (motor
add/edit, security, steps). The panel and the dashboard share `StatusSnapshot`,
the command queue, and the same Start gate.

## Hardware

| Item | Value |
|---|---|
| Display | SH1106 128×64 I²C, default address **0x3C** |
| Display bus | **Second** I²C bus `Wire1` — GPIO 25 SDA / 47 SCL |
| Encoder | KY-040, GPIO 22 CLK / 23 DT / 24 SW |
| Buzzer | existing LEDC buzzer on GPIO 21 |

The display bus is physically separate from the ADS1115 bus (`Wire` GPIO 14/42),
so no mutex is shared with `voltage.cpp` and `voltage.cpp` is unchanged except for
two read-only accessors.

KY-040 wiring: the module has its own pull-ups on CLK/DT/SW, so the pins are
configured as plain `INPUT`. No internal pull-ups, no external resistors.

## Library

U8g2, using `U8G2_SH1106_128X64_NONAME_F_2ND_HW_I2C` (the variant that targets
`Wire1`). Initialization:

```
Wire1.begin(OLED_SDA_PIN, OLED_SCL_PIN);
Wire1.setClock(OLED_I2C_HZ);
u8g2.begin();
Wire1.begin(OLED_SDA_PIN, OLED_SCL_PIN);   // re-bind; U8g2 may call begin() with no pins
Wire1.setClock(OLED_I2C_HZ);
```

Never call `Wire1.end()`. The primary `Wire` bus is never touched.

Fallback: only if `Wire1` is genuinely unavailable on the target, switch the
constructor to `U8G2_SH1106_128X64_NONAME_F_SW_I2C` with the same pins. Do not
use the bit-banged variant to work around a hardware-constructor signature — the
point of this change is a real second I2C peripheral isolated from the ADS1115
bus.

Fonts: a proportional/bold face (`helvB08`, `helvB10`) for titles and headers, a
proportional body face (`6x12`), and a small face (`5x7`) for dense lines.

## Task model

`uiTask` is pinned to **core 0**, priority 2, 8 KB stack, and is **not** on the
Task WDT. `loop()` and `protectionTask` stay on core 1. `uiTask` reads motor
state only through `protectionSnapshot()` and `protectionCopyLog()`. It never
samples ADC/ADS and never writes relays.

The loop polls the encoder every ~3 ms; the display refreshes every ~160 ms or on
an event.

## Input model

| Input | Home | Per-motor | Fault Log / Diag / FW / Network |
|---|---|---|---|
| Rotate | Move selection | Next/previous motor | Scroll |
| Short press | Open row (motor or menu) | Toggle Start/Stop (gated) | No-op |
| Long press (~600 ms) | No-op | Back to Home | Back to Home |

Buzzer:

- One `toneClick()` per confirmed detent and per Home "open".
- `toneBack()` on long-press back (two-note descending blip).
- Start/Stop success tone comes from `protectionTask` (`TONE_STARTED` /
  `TONE_STOPPED`); the panel does not play it a second time.
- Rejected Start shows the reason and plays `toneFault()`.

The sequencer in `buzzer.cpp` is guarded by a mutex because `uiTask` (core 0) and
`loop()` (core 1) both issue tones.

### Quadrature decoding

A 16-entry state table over `(prev<<2)|current` accumulates to ±4 before emitting
one step, so a single detent produces exactly one event despite KY-040 bounce.
Switch debounce is 40 ms; short vs long press is split at 600 ms.

## Screens

All screens share: a slim header (title + fault-count badge + SoftAP icon), a
thin right-edge scrollbar when content overflows, inverted (black-on-white)
selection, and a segmented thermal bar.

1. **Boot splash** — wipe-reveal of the project name, then a live checklist of
   relay fail-safe, ADS1115 probe, SD mount, calibration. `setup()` publishes
   stages through `uiBootStage()` / `uiBootNote()`; the splash auto-advances to
   Home ~1.2 s after `UI_BOOT_DONE`. No input.
2. **Home** — one compact row per configured motor (status icon + name), then
   Fault Log / Diagnostics / Firmware Info / Network Info.
3. **Per-motor** — status icon (+ fault-type text when Fault), live current,
   segmented thermal bar, live DC voltage / power (or rated V / VA).
4. **Fault Log** — most recent first, from the non-destructive 16-entry ring.
5. **Diagnostics** — 0x48/0x49 found/missing with the I²C error label, per-channel
   `v_calibrated`, SD mounted, free heap.
6. **Firmware Info** — `__DATE__` / `__TIME__` and uptime.
7. **Network Info** — SoftAP SSID/password, AP IP, dashboard login, read from
   `config_limits.h` and `motorStoreAuthGet()`.

## Status representation

- Four 8×8 XBM icons, one per `MotorStatus`: stopped square, running triangle,
  fault warning triangle, cooling snowflake. Used everywhere a status is shown.
- A `FaultType` never gets its own icon. It is the fault warning triangle plus a
  short word: `I2T`, `STALL`, `SENSOR`, `UNDER-V`, `OVER-V`, `NO-I`.
- A small AP glyph lives in the header.

## Safety-critical shared gate

The DC-readiness check previously lived inline in `web.cpp`'s `handleCmd()`.
It is extracted to:

```c
bool protectionCanStart(const StatusSnapshot &snap, int idx, const char **reason);
```

Both the web API and the panel call it before posting `CMD_START`. When it
returns false the panel renders `*reason` and plays `toneFault()`. The reasons
are unchanged:

- `DC start needs the ADS1115 for this motor's channels`
- `calibrate DC voltage zeros first`

`protectionTask` keeps its own status/`s_calibrated` checks, so the gate is
advisory but the queue path is still authoritative.

## Non-destructive trip ring

`protectionPopLog()` is a destructive single-consumer queue drained by `loop()`
into the SD CSV. The panel must not steal from it. `pushLog()` now also appends
to a 16-entry RAM ring under `s_mu`, exposed through:

```c
int protectionCopyLog(LogEvent *out, int max);
```

`out` is filled most-recent-first and the count returned. The SD queue's
behaviour is unchanged.

## Files

New: `ui.h`, `ui.cpp`, `ui_icons.h`.

Changed: `config_pins.h` (panel pins), `types.h` (`TONE_BACK`), `buzzer.h` /
`buzzer.cpp` (`toneBack()`, sequencer mutex), `voltage.h` / `voltage.cpp`
(`voltageAdsErr()`, `voltageI2cErrLabel()`), `protection.h` / `protection.cpp`
(`protectionCanStart()`, `protectionCopyLog()`), `web.cpp` (use the shared gate),
`MotorProtection.ino` (`uiBegin()`, boot stages).

## Verification constraints

This environment has no Arduino-ESP32 toolchain, so the sketch is not compiled
here. Bench checks are listed in `AGENTS.md` under "Next planned steps".
