#include <Arduino.h>
#include "relays.h"
#include "config_pins.h"
#include "config_limits.h"

void relaysBegin() {
  for (int i = 0; i < MAX_CHANNELS; i++) {
    pinMode(PIN_RELAY[i], OUTPUT);
    digitalWrite(PIN_RELAY[i], LOW);
  }
  Serial.println("RELAY: all OFF (default active-HIGH, GPIO LOW = de-energized)");
}

void relaysSetChannel(int ch, bool energized, uint8_t active_high) {
  if (ch < 0 || ch >= MAX_CHANNELS) {
    return;
  }
  const bool high = energized ? (active_high != 0) : (active_high == 0);
  digitalWrite(PIN_RELAY[ch], high ? HIGH : LOW);
}

void relaysMotorOff(const uint8_t *channels, const uint8_t *active_high, uint8_t phase_count) {
  for (uint8_t p = 0; p < phase_count; p++) {
    if (channels[p] != CH_UNUSED) {
      relaysSetChannel(channels[p], false, active_high[p]);
    }
  }
}

void relaysMotorOn(const uint8_t *channels, const uint8_t *active_high, uint8_t phase_count) {
  for (uint8_t p = 0; p < phase_count; p++) {
    if (channels[p] != CH_UNUSED) {
      relaysSetChannel(channels[p], true, active_high[p]);
    }
  }
}

void relaysLogPolarity(const uint8_t *active_high_by_ch) {
  Serial.print("RELAY: polarity");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    const uint8_t ah = active_high_by_ch ? active_high_by_ch[i] : 1;
    Serial.printf(" CH%d=%s", i, ah ? "HIGH" : "LOW");
  }
  Serial.println();
}

void relaysDeenergizeAll(const uint8_t *active_high_by_ch) {
  for (int i = 0; i < MAX_CHANNELS; i++) {
    const uint8_t ah = active_high_by_ch ? active_high_by_ch[i] : 1;
    relaysSetChannel(i, false, ah);
  }
  relaysLogPolarity(active_high_by_ch);
}
