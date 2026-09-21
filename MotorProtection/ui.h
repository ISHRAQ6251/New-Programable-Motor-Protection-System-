#pragma once

#include <stdint.h>

// Local status/control panel: SH1106 128x64 OLED on the shared ADS1115 I2C
// bus (Wire, GPIO 14/42), KY-040 encoder (CLK 47 / DT 46 / SW 19), and
// onboard WS2812 on GPIO 48 (one-wire, never s_i2c_mu). Runs in its own
// task on core 0 and reads motor state only through the mutex-protected
// protectionSnapshot()/protectionCopyLog() APIs.

enum UiBootStage : uint8_t {
  UI_BOOT_SPLASH = 0,
  UI_BOOT_RELAYS,
  UI_BOOT_ADS,
  UI_BOOT_SD,
  UI_BOOT_CAL,
  UI_BOOT_DONE
};

void uiBegin();
void uiTask(void *arg);

// Called from setup() as each early boot step completes; the splash polls this.
void uiBootStage(UiBootStage stage);
void uiBootNote(const char *text);
