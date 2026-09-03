#include <Arduino.h>
#include "buzzer.h"
#include "config_pins.h"
#include "types.h"

struct ToneStep {
  uint16_t freq_hz;
  uint16_t dur_ms;
};

static const ToneStep kPowerUp[] = {{523, 120}, {659, 120}, {784, 180}};
static const ToneStep kAdded[]   = {{880, 80}, {0, 40}, {880, 80}};
static const ToneStep kStarted[] = {{440, 60}, {880, 120}};
static const ToneStep kStopped[] = {{880, 60}, {440, 120}};
static const ToneStep kClick[]   = {{2000, 20}};
static const ToneStep kFault[]   = {{1000, 400}, {0, 200}};

static const ToneStep *s_seq = nullptr;
static int s_len = 0;
static int s_idx = 0;
static bool s_loop = false;
static uint32_t s_deadline = 0;
static bool s_playing = false;

static void applyFreq(uint16_t freq) {
  if (freq == 0) {
    ledcWrite(PIN_BUZZER, 0);
  } else {
    ledcChangeFrequency(PIN_BUZZER, freq, 10);
    ledcWrite(PIN_BUZZER, 512);
  }
}

static void startSeq(const ToneStep *seq, int len, bool loop) {
  s_seq = seq;
  s_len = len;
  s_idx = 0;
  s_loop = loop;
  s_playing = len > 0;
  if (!s_playing) {
    applyFreq(0);
    return;
  }
  applyFreq(seq[0].freq_hz);
  s_deadline = millis() + seq[0].dur_ms;
}

void buzzerBegin() {
  ledcAttach(PIN_BUZZER, 1000, 10);
  ledcWrite(PIN_BUZZER, 0);
}

void buzzerTick() {
  if (!s_playing) {
    return;
  }
  if ((int32_t)(millis() - s_deadline) < 0) {
    return;
  }
  s_idx++;
  if (s_idx >= s_len) {
    if (s_loop) {
      s_idx = 0;
    } else {
      s_playing = false;
      applyFreq(0);
      return;
    }
  }
  applyFreq(s_seq[s_idx].freq_hz);
  s_deadline = millis() + s_seq[s_idx].dur_ms;
}

void tonePowerUp()      { startSeq(kPowerUp, 3, false); }
void toneFault()        { startSeq(kFault, 2, true); }
void toneMotorAdded()   { startSeq(kAdded, 3, false); }
void toneMotorStarted() { startSeq(kStarted, 2, false); }
void toneMotorStopped() { startSeq(kStopped, 2, false); }
void toneClick()        { startSeq(kClick, 1, false); }
void toneSilence()      { s_playing = false; s_loop = false; applyFreq(0); }

void buzzerRequest(ToneId id) {
  switch (id) {
    case TONE_POWERUP:  tonePowerUp(); break;
    case TONE_FAULT:    toneFault(); break;
    case TONE_ADDED:    toneMotorAdded(); break;
    case TONE_STARTED:  toneMotorStarted(); break;
    case TONE_STOPPED:  toneMotorStopped(); break;
    case TONE_CLICK:    toneClick(); break;
    case TONE_SILENCE:  toneSilence(); break;
    default: break;
  }
}
