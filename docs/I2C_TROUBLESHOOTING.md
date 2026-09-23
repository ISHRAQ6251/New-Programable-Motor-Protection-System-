# I²C troubleshooting — ADS1115 on GPIO 14 / 42

Bench notes for the MPS-505 DC voltage bus. Firmware never auto-repairs a wiring swap.

## Bus (locked)

| Signal | ESP32-S3 GPIO | Notes |
|---|---|---|
| SDA | **14** | Not ESP32 default 8 (that is I-sense CH6) |
| SCL | **42** | Not ESP32 default 9 (that is I-sense CH7) |
| Clock | 400 kHz | `MPS_I2C_HZ` |
| Pull-ups | 4.7 kΩ–10 kΩ to **3.3 V** | Required if the ADS breakouts do not already have them |

ADS modules and the SH1106 OLED at 0x3C share this bus (`s_i2c_mu` serializes every Wire transaction). Power ADS VDD from 3.3 V, not 5 V.

## ADDR map (fixed in firmware)

| Module | ADDR pin | Address | Logical channels |
|---|---|---|---|
| ADS #1 | GND | **0x48** | CH4–CH7 → AIN0–AIN3 |
| ADS #2 | VDD | **0x49** | CH0–CH3 → AIN0–AIN3 |

Swapping the two physical boards does **not** swap channels. A mixed-up ADDR pin shows as the wrong chip missing.

## `Wire.end()` bus destruction

Do **not** call `Wire.end()`. On Arduino-ESP32 3.x it tears down the driver. A later `Wire.begin()` does not fully recover: scans hang, `endTransmission` returns timeout (`err=5`), or the bus prints `(none)` even with chips powered.

Firmware:

- `i2cInitOnce()` — `Wire.begin(14, 42)` **once**
- `voltageReprobe()` — scan + probe at Calibrate; no `end()`
- After `ads.begin()`, re-bind `Wire.begin(14, 42)` because Adafruit BusIO may call `Wire.begin()` with no pins

If an older sketch called `Wire.end()` between chips, power-cycle the board after flashing this tree.

## SDA / SCL swap

Firmware cannot detect a swap. Symptoms:

- Boot `I2C: scan (none)`
- Both chips `ADS: 0x48/0x49 not found (I2C err=5 timeout)` or `err=2 NACK addr`
- 5 s heartbeat `I2C: diag ads_ok=[0,0] …`

Check on the ADS modules (not the ESP32 silk for 8/9): GPIO 14 goes to SDA, GPIO 42 to SCL. Swap the two Dupont wires and press **Calibrate** (or reset). A correct bus prints `I2C: scan 0x48 0x49` (and `0x3C` if an OLED is fitted).

## Pull-ups

Missing or 5 V pull-ups cause timeouts (`err=5`) or flaky ACK. Both SDA and SCL need a path to 3.3 V. Many ADS1115 breakouts already have 10 kΩ; two modules in parallel is fine. Do not add a second pair to 5 V.

## `i2cScan()` / Serial

At `voltageBegin()` and each Calibrate re-probe:

```
I2C: initialized SDA=14 SCL=42 clock=400000Hz
I2C: scan 0x48 0x49
I2C: expected 0x48 (ADDR->GND)=CH4-7, 0x49 (ADDR->VDD)=CH0-3
ADS: 0x48 ok GAIN_ONE 128SPS
ADS: 0x49 ok GAIN_ONE 128SPS
```

`loop()` prints at most every 5 s while a chip or channel zero is missing:

```
I2C: diag ads_ok=[1,0] vcal=[1,1,1,1,0,0,0,0]
```

## `endTransmission` codes

| Code | Label | Typical cause |
|---|---|---|
| 0 | ok | Device ACKed |
| 1 | data too long | Buffer (should not happen on a probe) |
| 2 | NACK addr | Nothing at that address; ADDR pin wrong; unpowered chip |
| 3 | NACK data | Rare on a zero-byte probe |
| 4 | other | Driver error |
| 5 | timeout | Stuck bus, SDA/SCL swap, missing pull-ups, `Wire.end()` leftover |

## Conversion timeout

`readAdcVolts()` starts a conversion under the mutex, releases the bus while the ADS converts, then reacquires it only for short status/result reads. At 128 SPS the nominal conversion is 7.8 ms; firmware waits 10 ms before polling and allows 35 ms for completion. Timeout → that sample is treated as missing (`present=0`). A running DC motor then counts a voltage-path sensor fault; `SENSOR_FAULT` trips only after 3 consecutive voltage faults within 1 s (skipped while jam-release pulses run).

When a DC `SENSOR_FAULT` occurs, the firmware prints `VOLT_DIAG` lines containing the
logical channel, ADS address/AIN, averaged ADC voltage, calibration zero, bus voltage,
sample validity, and timeout count. `VOLT_DIAG_SAMPLE` contains the four samples from the
faulting voltage window; `VOLT_DIAG_HISTORY` contains recent raw-read entries from the
per-channel RAM diagnostic ring. These diagnostics do not change trip behavior and do not
modify the installed Adafruit libraries.

## Calibrate vs reboot

ADDR / Dupont fixes are picked up by **Calibrate zeros** (`voltageReprobe()`), not only by a power cycle. Relays stay off so voltage taps sit at 0 V.

## Quick checklist

1. GPIO 14 = SDA, 42 = SCL (not 8/9)
2. ADDR: one module GND (0x48), the other VDD (0x49)
3. 3.3 V VDD and GND common with the ESP32
4. Pull-ups on SDA/SCL to 3.3 V
5. Serial scan lists 0x48 and/or 0x49
6. No `Wire.end()` in the sketch
