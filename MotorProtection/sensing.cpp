#include <Arduino.h>
#include <math.h>
#include "sensing.h"
#include "config_pins.h"
#include "config_limits.h"

void sensingBegin() {
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    pinMode(PIN_ISENSE[i], INPUT);
    analogSetPinAttenuation(PIN_ISENSE[i], ADC_11db);
  }
}

float sensingAdcToAmps(int millivolts, float zero_mv) {
  return ((float)millivolts - zero_mv) / MV_PER_AMP;
}

void sensingCalibrateAll(ChannelRuntime *ch) {
  for (int i = 0; i < MAX_CHANNELS; i++) {
    analogRead(PIN_ISENSE[i]);
    delay(2);
    uint32_t acc = 0;
    for (int n = 0; n < CAL_SAMPLES; n++) {
      acc += (uint32_t)analogReadMilliVolts(PIN_ISENSE[i]);
      delayMicroseconds(200);
    }
    ch[i].zero_mv = (float)acc / (float)CAL_SAMPLES;
    ch[i].last_rms = 0;
    ch[i].energy_a2s = 0;
    ch[i].last_ms = 0;
    ch[i].calibrated = 1;
    Serial.printf("CAL: CH%d i_zero=%.1f mV\n", i, (double)ch[i].zero_mv);
  }
}

SampleResult sensingSample(int ch, const ChannelRuntime *rt, uint8_t is_ac, uint8_t mains_hz) {
  SampleResult r;
  r.rms = 0;
  r.mean_v = 0;
  r.stuck = 1;
  r.out_of_range = 0;
  if (ch < 0 || ch >= MAX_CHANNELS) {
    r.out_of_range = 1;
    return r;
  }

  const uint32_t window_us = (is_ac && mains_hz == 60) ? 16667u : 20000u;
  const uint32_t t0 = micros();
  int n = 0;
  double sum_i2 = 0;
  double sum_abs = 0;
  double sum_mv = 0;
  int first_raw = -1;

  while (n < RMS_SAMPLES || (micros() - t0) < window_us) {
    const int mv = analogReadMilliVolts(PIN_ISENSE[ch]);
    if (first_raw < 0) {
      first_raw = mv;
    } else if (mv != first_raw) {
      r.stuck = 0;
    }
    const float i = sensingAdcToAmps(mv, rt->zero_mv);
    sum_i2 += (double)i * (double)i;
    sum_abs += fabs((double)i);
    sum_mv += (double)mv;
    n++;
    if (n >= 256) {
      break;
    }
  }

  if (n <= 1) {
    r.stuck = 1;
  }
  if (r.stuck) {
    Serial.printf("SENS: CH%d stuck ADC raw=%d n=%d\n", ch, first_raw, n);
  }

  const float mean_mv = (n > 0) ? (float)(sum_mv / (double)n) : 0;
  r.mean_v = mean_mv / 1000.0f;
  if (r.mean_v < VADC_MIN || r.mean_v > VADC_MAX) {
    r.out_of_range = 1;
  }

  if (is_ac) {
    r.rms = (n > 0) ? sqrtf((float)(sum_i2 / (double)n)) : 0;
  } else {
    r.rms = (n > 0) ? (float)(sum_abs / (double)n) : 0;
  }
  return r;
}
