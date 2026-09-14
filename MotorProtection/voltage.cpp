#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_ADS1X15.h>
#include "voltage.h"
#include "config_pins.h"
#include "config_limits.h"

static Adafruit_ADS1115 s_ads[2];
static uint8_t s_ok[2] = {0, 0};
static const uint8_t kAddr[2] = {ADS1115_ADDR_A, ADS1115_ADDR_B};

static bool probeAddr(uint8_t addr) {
  voltageRebindWire();
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void voltageRebindWire() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
}

void voltageBegin() {
  s_ok[0] = 0;
  s_ok[1] = 0;
  voltageRebindWire();
  Wire.setClock(400000);

  for (int i = 0; i < 2; i++) {
    voltageRebindWire();
    const bool begun = s_ads[i].begin(kAddr[i], &Wire);
    Wire.end();
    voltageRebindWire();
    Wire.setClock(400000);
    if (!begun || !probeAddr(kAddr[i])) {
      Serial.printf("ADS1115 0x%02X: not found\n", kAddr[i]);
      s_ok[i] = 0;
      continue;
    }
    s_ads[i].setGain(GAIN_ONE);
    s_ads[i].setDataRate(RATE_ADS1115_250SPS);
    s_ok[i] = 1;
    Serial.printf("ADS1115 0x%02X: ok GAIN_ONE 250SPS\n", kAddr[i]);
  }
}

bool voltageAdsOk(int chip) {
  return chip >= 0 && chip < 2 && s_ok[chip];
}

static int chipOf(int ch) {
  return (ch < 4) ? 0 : 1;
}

static int ainOf(int ch) {
  return ch & 3;
}

static bool readAdcVolts(int chip, int ain, float *out_v) {
  if (!s_ok[chip] || ain < 0 || ain > 3 || !out_v) {
    return false;
  }
  s_ads[chip].startADCReading(MUX_BY_CHANNEL[ain], false);
  const uint32_t t0 = millis();
  while (!s_ads[chip].conversionComplete()) {
    if ((uint32_t)(millis() - t0) > 20) {
      return false;
    }
    delay(1);
  }
  const int16_t raw = s_ads[chip].getLastConversionResults();
  *out_v = s_ads[chip].computeVolts(raw);
  return true;
}

void voltageCalibrateAll(ChannelRuntime *ch) {
  voltageRebindWire();
  for (int i = 0; i < MAX_CHANNELS; i++) {
    ch[i].last_v = 0;
    ch[i].v_calibrated = 0;
    ch[i].v_zero = 0;
    const int chip = chipOf(i);
    if (!s_ok[chip]) {
      continue;
    }
    double acc = 0;
    int n = 0;
    for (int k = 0; k < V_CAL_SAMPLES; k++) {
      float v = 0;
      if (!readAdcVolts(chip, ainOf(i), &v)) {
        continue;
      }
      acc += (double)v;
      n++;
    }
    if (n > 0) {
      ch[i].v_zero = (float)(acc / (double)n);
      ch[i].v_calibrated = 1;
    }
  }
}

VoltageSample voltageSample(int ch, const ChannelRuntime *rt) {
  VoltageSample r;
  r.v_bus = 0;
  r.v_adc = 0;
  r.present = 0;
  r.fault = 1;
  if (ch < 0 || ch >= MAX_CHANNELS || !rt) {
    return r;
  }
  const int chip = chipOf(ch);
  if (!s_ok[chip]) {
    return r;
  }
  float vadc = 0;
  if (!readAdcVolts(chip, ainOf(ch), &vadc)) {
    return r;
  }
  r.present = 1;
  r.v_adc = vadc;
  r.v_bus = (r.v_adc - rt->v_zero) * VDIV_SCALE;
  r.fault = 0;
  if (r.v_adc > VADC_ABS_MAX || r.v_adc < -0.05f || fabsf(r.v_bus) > VBUS_CAP) {
    r.fault = 1;
  }
  return r;
}
