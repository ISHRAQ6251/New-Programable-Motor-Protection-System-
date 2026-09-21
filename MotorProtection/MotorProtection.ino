/*
 * MPS-505 Programmable Motor Protection
 * Board: ESP32-S3-N16R8
 *
 * Arduino IDE:
 *   Board: "ESP32S3 Dev Module"
 *   Flash Size: 16MB (128Mb)
 *   PSRAM: OPI PSRAM
 *   USB CDC On Boot: Enabled (optional)
 *   Core Debug Level: Debug  (heap poisoning / extra checks)
 *
 * Libraries (Library Manager):
 *   ESPAsyncWebServer  (ESP32Async)
 *   AsyncTCP           (ESP32Async)
 *   Adafruit ADS1X15
 *   Adafruit BusIO     (dependency of ADS1X15)
 *   U8g2               (for the local SH1106 OLED panel)
 *   Adafruit NeoPixel  (onboard WS2812 status LED on GPIO 48)
 *
 * SoftAP: MPS-505 / mps50005   dashboard: http://192.168.4.1
 * Default dashboard login: mps / mps500  (themed /login page, session cookie)
 */

#include <Arduino.h>
#include "config_pins.h"
#include "config_limits.h"
#include "relays.h"
#include "buzzer.h"
#include "sensing.h"
#include "voltage.h"
#include "motor_store.h"
#include "protection.h"
#include "sd_log.h"
#include "net_ap.h"
#include "web.h"
#include "ui.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("MPS-505 boot...");
  Serial.flush();

  relaysBegin();
  buzzerBegin();
  uiBegin();
  sensingBegin();
  voltageBegin();
  {
    char note[48];
    snprintf(note, sizeof(note), "ADS 0x48 %s / 0x49 %s",
             voltageAdsOk(0) ? "ok" : "missing",
             voltageAdsOk(1) ? "ok" : "missing");
    uiBootNote(note);
  }
  motorStoreBegin();
  {
    MotorRecord motors[MAX_MOTORS];
    uint8_t pol[MAX_CHANNELS];
    for (int i = 0; i < MAX_CHANNELS; i++) {
      pol[i] = 1;
    }
    motorStoreGet(motors);
    for (int i = 0; i < MAX_MOTORS; i++) {
      if (!motors[i].used) {
        continue;
      }
      for (int p = 0; p < motors[i].phase_count; p++) {
        const uint8_t c = motors[i].channels[p];
        if (c < MAX_CHANNELS) {
          pol[c] = motors[i].relay_active_high[p];
        }
      }
    }
    relaysDeenergizeAll(pol);
  }
  uiBootStage(UI_BOOT_RELAYS);
  uiBootStage(UI_BOOT_ADS);
  sdLogBegin();
  uiBootStage(UI_BOOT_SD);
  protectionBegin();
  protectionSetSdOk(sdLogOk() ? 1 : 0);
  uiBootStage(UI_BOOT_CAL);

  netApBegin();
  webBegin();

  xTaskCreatePinnedToCore(protectionTask, "protect", 8192, nullptr, 5, nullptr, 1);

  char user[AUTH_USER_LEN];
  char pass[AUTH_PASS_LEN];
  motorStoreAuthGet(user, pass);
  netApPrintBanner(user, pass);
  Serial.printf("HEAP: boot=%u\n", (unsigned)ESP.getFreeHeap());

  uiBootStage(UI_BOOT_DONE);
  tonePowerUp();
}

void loop() {
  ToneId t = protectionTakeTone();
  if (t != TONE_NONE) {
    buzzerRequest(t);
  }
  buzzerTick();

  LogEvent ev;
  while (protectionPopLog(&ev)) {
    sdLogAppend(&ev);
    protectionSetSdOk(sdLogOk() ? 1 : 0);
  }

  static uint32_t diag_last = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - diag_last) >= 5000u) {
    diag_last = now;
    const uint8_t a0 = voltageAdsOk(0) ? 1 : 0;
    const uint8_t a1 = voltageAdsOk(1) ? 1 : 0;
    uint8_t vcal[MAX_CHANNELS];
    protectionCopyVcal(vcal);
    bool ready = a0 && a1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
      if (!vcal[i]) {
        ready = false;
        break;
      }
    }
    if (!ready) {
      Serial.printf("I2C: diag ads_ok=[%u,%u] vcal=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
                    (unsigned)a0, (unsigned)a1,
                    (unsigned)vcal[0], (unsigned)vcal[1],
                    (unsigned)vcal[2], (unsigned)vcal[3],
                    (unsigned)vcal[4], (unsigned)vcal[5],
                    (unsigned)vcal[6], (unsigned)vcal[7]);
    }
  }

  delay(5);
}
