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
#include "motor_store.h"
#include "protection.h"
#include "sd_log.h"
#include "net_ap.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  delay(200);

  relaysBegin();
  buzzerBegin();
  sensingBegin();
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
  sdLogBegin();
  protectionBegin();
  protectionSetSdOk(sdLogOk() ? 1 : 0);

  netApBegin();
  webBegin();

  xTaskCreatePinnedToCore(protectionTask, "protect", 8192, nullptr, 5, nullptr, 1);

  char user[AUTH_USER_LEN];
  char pass[AUTH_PASS_LEN];
  motorStoreAuthGet(user, pass);
  netApPrintBanner(user, pass);
  Serial.printf("heap boot=%u\n", (unsigned)ESP.getFreeHeap());

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

  delay(5);
}
