# I2C contention and ADS1115 code review

Date: 2026-09-22

## Root causes found

1. `readAdcVolts()` released `s_i2c_mu` between conversion-complete polls, but the 20 ms deadline was too close to a complete SH1106 transfer. A full 128x64 frame is 1024 data bytes. At 400 kHz, the wire time is approximately `(1024 data bytes + 8 control/address bytes) * 9 bits / 400000 = 23.2 ms`, before start/stop and command overhead. A frame therefore can occupy the mutex for roughly 23–25 ms. `conversionComplete()` reads the ADS config register over I2C, so a poll delayed behind `sendBuffer()` could time out and return `present=0`; `applySample()` then trips `FT_SENSOR`.
2. Logical voltage readiness in `protection.cpp` used the old chip split even after the bench wiring was identified as swapped.
3. 250 SPS was unnecessary for the DC protection loop and increased conversion polling/bus activity. Live firmware uses 128 SPS.

The contention was not a permanent priority inversion: the mutex is a FreeRTOS mutex with priority inheritance, and the tasks run on different cores. The protection task can still wait for the bounded OLED transaction, however. The timeout race was real.

## Task interaction map

| Task | Core / priority | I2C devices | Mutex hold | Frequency |
|---|---:|---|---|---|
| `protectionTask` | 1 / 5 | ADS1115 0x48/0x49 | Start, status, and result calls are separate short critical sections; conversion wait is outside the mutex | Each running motor, 4 voltage samples per reading |
| `uiTask` | 0 / 2 | SH1106 0x3C | `sendBuffer()` holds the mutex for about 23–25 ms worst case | Every 160 ms |
| `setup()` / calibration path | setup, then protection context | ADS1115 scan/probe/calibration | Each Wire/library operation is mutex-protected; conversion waits are outside it | Boot and explicit Calibrate |

`loop()` does not access I2C directly. No `Wire.end()` call exists in the firmware.

## Blocking-call audit

- `i2cProbe()` / `i2cScan()`: one Wire transaction per address, mutex held only for `endTransmission()`. A full scan is many short transactions.
- `ads.begin()`, `setGain()`, and `setDataRate()`: library register transactions under the mutex during boot/reprobe.
- `readAdcVolts()`: starts conversion under the mutex, waits 10 ms with the mutex released, then polls `conversionComplete()` in short mutex sections until the 35 ms deadline, and finally reads the result in another short section. No single I2C critical section approaches 100 ms.
- `s_oled.sendBuffer()`: the only long shared-bus critical section. At 400 kHz, 1024 payload bytes require 9216 clock bits, or 23.04 ms; commands, address/control bytes, ACKs, and transaction boundaries make the practical upper bound approximately 23–25 ms.

## SENSOR_FAULT trace

`applySample()` sets `FT_SENSOR` for current out-of-range/stuck samples on that window (while `jam_phase == JAM_IDLE`), or for a DC `VoltageSample` that is absent/faulted after 3 consecutive voltage faults within 1 s. `voltageSample()` returns `present=0` when every `readAdcVolts()` attempt fails. Before this change, an ADS conversion-complete poll could miss its 20 ms deadline while the OLED held the mutex. That path is now mitigated by waiting for the 128 SPS conversion with the bus released and using a 35 ms deadline. Remaining voltage-path timeouts still count toward the 3/1 s debounce. Electrical faults remain intentionally trip-worthy. All sensor-fault checks are skipped while jam-release pulses run.

## Channel mapping

The authoritative software map is now:

| Logical channel | ADS address | AIN |
|---|---:|---:|
| CH0–CH3 | 0x49 | AIN0–AIN3 |
| CH4–CH7 | 0x48 | AIN0–AIN3 |

`chipOf()` and `ainOf()` are used by calibration and sampling. `protection.cpp` uses the same split for the start-readiness gate. Web and OLED code carry logical channel numbers only and therefore require no separate address mapping.

## Sample-rate decision

`RATE_ADS1115_128SPS` gives a nominal 7.8 ms conversion. The firmware waits 10 ms before status polling and allows 35 ms total from conversion start for completion. The existing four-sample average and 750 ms UV/OV grace make this rate appropriate for voltage changes occurring over seconds or minutes.

## Additional risks

- A missing ADS or a genuine stuck I2C bus still produces `present=0` and an intentional sensor trip.
- Calibration remains sequential and can take longer at 128 SPS, but it runs only while motors are idle.
- The OLED frame transfer is still a long mutex holder; it is bounded and now has conversion-time headroom, but a hardware RDY pin or a separate bus would further reduce latency if future requirements become tighter.
