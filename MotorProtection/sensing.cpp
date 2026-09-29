#include <Arduino.h>
#include <math.h>
#include <string.h>
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

void sensingResetAcWindow(ChannelRuntime *ch) {
  if (!ch) {
    return;
  }
  memset(ch->ac_sum_i2, 0, sizeof(ch->ac_sum_i2));
  memset(ch->ac_n, 0, sizeof(ch->ac_n));
  ch->ac_cycles = 0;
  ch->ac_head = 0;
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
    sensingResetAcWindow(&ch[i]);
    Serial.printf("CAL: CH%d i_zero=%.1f mV\n", i, (double)ch[i].zero_mv);
  }
}

float sensingPushAcCycle(ChannelRuntime *ch, double sum_i2, int n) {
  if (!ch) {
    return 0;
  }
  if (n <= 0) {
    return 0;
  }
  const uint8_t head = ch->ac_head % (uint8_t)AC_AVG_CYCLES;
  ch->ac_sum_i2[head] = sum_i2;
  ch->ac_n[head] = (n > 65535) ? 65535 : (uint16_t)n;
  ch->ac_head = (uint8_t)((head + 1) % AC_AVG_CYCLES);
  if (ch->ac_cycles < AC_AVG_CYCLES) {
    ch->ac_cycles++;
  }
  double tot_i2 = 0;
  uint32_t tot_n = 0;
  for (uint8_t i = 0; i < ch->ac_cycles; i++) {
    tot_i2 += ch->ac_sum_i2[i];
    tot_n += ch->ac_n[i];
  }
  if (tot_n == 0) {
    return 0;
  }
  return sqrtf((float)(tot_i2 / (double)tot_n));
}

static void accumulateOne(int ch, const ChannelRuntime *rt, SampleResult *r,
                          int *first_raw, int *n, double *sum_i2, double *sum_abs,
                          double *sum_mv) {
  const int mv = analogReadMilliVolts(PIN_ISENSE[ch]);
  if (*first_raw < 0) {
    *first_raw = mv;
  } else if (mv != *first_raw) {
    r->stuck = 0;
  }
  const float i = sensingAdcToAmps(mv, rt->zero_mv);
  *sum_i2 += (double)i * (double)i;
  *sum_abs += fabs((double)i);
  *sum_mv += (double)mv;
  (*n)++;
}

static void finishSample(SampleResult *r, int n, double sum_i2, double sum_abs,
                         double sum_mv, uint8_t is_ac, int first_raw, int ch) {
  if (n <= 1) {
    r->stuck = 1;
  }
  if (r->stuck) {
    Serial.printf("SENS: CH%d stuck ADC raw=%d n=%d\n", ch, first_raw, n);
  }

  const float mean_mv = (n > 0) ? (float)(sum_mv / (double)n) : 0;
  r->mean_v = mean_mv / 1000.0f;
  if (r->mean_v < VADC_MIN || r->mean_v > VADC_MAX) {
    r->out_of_range = 1;
  }

  r->sum_i2 = sum_i2;
  r->n = n;
  if (is_ac) {
    r->rms = (n > 0) ? sqrtf((float)(sum_i2 / (double)n)) : 0;
  } else {
    r->rms = (n > 0) ? (float)(sum_abs / (double)n) : 0;
  }
}

SampleResult sensingSample(int ch, const ChannelRuntime *rt, uint8_t is_ac, uint8_t mains_hz) {
  SampleResult r;
  r.rms = 0;
  r.mean_v = 0;
  r.stuck = 1;
  r.out_of_range = 0;
  r.sum_i2 = 0;
  r.n = 0;
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
    accumulateOne(ch, rt, &r, &first_raw, &n, &sum_i2, &sum_abs, &sum_mv);
    if (n >= 256) {
      break;
    }
  }

  finishSample(&r, n, sum_i2, sum_abs, sum_mv, is_ac, first_raw, ch);
  return r;
}

void sensingSampleAcChannels(const uint8_t *ch_list, int nch, const ChannelRuntime *rt,
                             uint8_t mains_hz, SampleResult *out_by_ch) {
  if (!ch_list || !rt || !out_by_ch || nch <= 0) {
    return;
  }

  int idx[MAX_CHANNELS];
  int nvalid = 0;
  for (int i = 0; i < nch && nvalid < MAX_CHANNELS; i++) {
    const uint8_t c = ch_list[i];
    if (c >= MAX_CHANNELS) {
      continue;
    }
    idx[nvalid++] = (int)c;
    out_by_ch[c].rms = 0;
    out_by_ch[c].mean_v = 0;
    out_by_ch[c].stuck = 1;
    out_by_ch[c].out_of_range = 0;
    out_by_ch[c].sum_i2 = 0;
    out_by_ch[c].n = 0;
  }
  if (nvalid == 0) {
    return;
  }

  const uint32_t window_us = (mains_hz == 60) ? 16667u : 20000u;
  int n[MAX_CHANNELS];
  double sum_i2[MAX_CHANNELS];
  double sum_abs[MAX_CHANNELS];
  double sum_mv[MAX_CHANNELS];
  int first_raw[MAX_CHANNELS];
  memset(n, 0, sizeof(n));
  memset(sum_i2, 0, sizeof(sum_i2));
  memset(sum_abs, 0, sizeof(sum_abs));
  memset(sum_mv, 0, sizeof(sum_mv));
  for (int i = 0; i < MAX_CHANNELS; i++) {
    first_raw[i] = -1;
  }

  const uint32_t t0 = micros();
  int rounds = 0;
  while (rounds < RMS_SAMPLES || (micros() - t0) < window_us) {
    for (int k = 0; k < nvalid; k++) {
      const int c = idx[k];
      if (n[c] >= 256) {
        continue;
      }
      accumulateOne(c, &rt[c], &out_by_ch[c], &first_raw[c], &n[c],
                    &sum_i2[c], &sum_abs[c], &sum_mv[c]);
    }
    rounds++;
    int all_capped = 1;
    for (int k = 0; k < nvalid; k++) {
      if (n[idx[k]] < 256) {
        all_capped = 0;
        break;
      }
    }
    if (all_capped) {
      break;
    }
  }

  for (int k = 0; k < nvalid; k++) {
    const int c = idx[k];
    finishSample(&out_by_ch[c], n[c], sum_i2[c], sum_abs[c], sum_mv[c], 1,
                 first_raw[c], c);
  }
}
