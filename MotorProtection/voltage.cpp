#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_ADS1X15.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "voltage.h"
#include "config_pins.h"
#include "config_limits.h"
#include "types.h"

static Adafruit_ADS1115 s_ads[2];
static uint8_t s_ok[2] = {0, 0};
static uint8_t s_err[2] = {0, 0};
static uint8_t s_i2c_initialized = 0;
static const uint8_t kAddr[2] = {ADS1115_ADDR_A, ADS1115_ADDR_B};
static SemaphoreHandle_t s_i2c_mu = nullptr;
static VoltageDiagnostic s_voltage_diag[MAX_CHANNELS];
static uint32_t s_voltage_timeout_count[2] = {0, 0};
static VoltageReadDiagnostic s_voltage_ring[MAX_CHANNELS][VOLTAGE_DIAG_RING];
static uint8_t s_voltage_ring_head[MAX_CHANNELS] = {0};
static uint8_t s_voltage_ring_count[MAX_CHANNELS] = {0};

static void recordVoltageRead(uint8_t logical_channel, int chip, int ain,
                              bool valid, int16_t raw, float vadc) {
  if (logical_channel >= MAX_CHANNELS || chip < 0 || chip >= 2 ||
      ain < 0 || ain > 3) {
    return;
  }
  VoltageDiagnostic &d = s_voltage_diag[logical_channel];
  d.timestamp_ms = millis();
  d.logical_channel = logical_channel;
  d.chip = (uint8_t)chip;
  d.address = kAddr[chip];
  d.ain = (uint8_t)ain;
  d.raw[0] = raw;
  d.sample_v_adc[0] = vadc;
  d.sample_valid[0] = valid ? 1 : 0;

  const uint8_t head = s_voltage_ring_head[logical_channel];
  VoltageReadDiagnostic &entry = s_voltage_ring[logical_channel][head];
  entry.timestamp_ms = d.timestamp_ms;
  entry.logical_channel = logical_channel;
  entry.chip = d.chip;
  entry.address = d.address;
  entry.ain = d.ain;
  entry.valid = valid ? 1 : 0;
  entry.raw = raw;
  entry.v_adc = vadc;
  s_voltage_ring_head[logical_channel] =
      (uint8_t)((head + 1) % VOLTAGE_DIAG_RING);
  if (s_voltage_ring_count[logical_channel] < VOLTAGE_DIAG_RING) {
    s_voltage_ring_count[logical_channel]++;
  }
}

void voltageI2cMutexInit() {
  if (!s_i2c_mu) {
    s_i2c_mu = xSemaphoreCreateMutex();
  }
}

void i2cLock() {
  xSemaphoreTake(s_i2c_mu, portMAX_DELAY);
}

void i2cUnlock() {
  if (s_i2c_mu) {
    xSemaphoreGive(s_i2c_mu);
  }
}

static void i2cBindPins() {
  i2cLock();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(MPS_I2C_HZ);
  i2cUnlock();
}

static void i2cInitOnce() {
  if (s_i2c_initialized) {
    return;
  }
  i2cBindPins();
  s_i2c_initialized = 1;
  Serial.printf("I2C: initialized SDA=%d SCL=%d clock=%uHz\n",
                PIN_I2C_SDA, PIN_I2C_SCL, (unsigned)MPS_I2C_HZ);
}

static uint8_t i2cProbe(uint8_t addr) {
  i2cLock();
  Wire.beginTransmission(addr);
  const uint8_t err = Wire.endTransmission();
  i2cUnlock();
  return err;
}

const char *voltageI2cErrLabel(uint8_t e) {
  switch (e) {
    case 0: return "ok";
    case 1: return "data too long";
    case 2: return "NACK addr";
    case 3: return "NACK data";
    case 4: return "other";
    case 5: return "timeout";
    default: return "unknown";
  }
}

static void i2cScan() {
  Serial.print("I2C: scan");
  uint8_t found = 0;
  for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
    if (i2cProbe(addr) == 0) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.print(" (none)");
  }
  Serial.println();
  Serial.printf("I2C: expected 0x%02X (ADDR->GND)=CH4-7, 0x%02X (ADDR->VDD)=CH0-3\n",
                ADS1115_ADDR_A, ADS1115_ADDR_B);
}

static bool adsInitChip(int i) {
  i2cLock();
  const bool begun = s_ads[i].begin(kAddr[i], &Wire);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(MPS_I2C_HZ);
  i2cUnlock();
  if (!begun) {
    return false;
  }
  i2cLock();
  s_ads[i].setGain(GAIN_ONE);
  s_ads[i].setDataRate(RATE_ADS1115_128SPS);
  i2cUnlock();
  return true;
}

void voltageBegin() {
  s_ok[0] = 0;
  s_ok[1] = 0;
  i2cInitOnce();
  i2cScan();

  for (int i = 0; i < 2; i++) {
    const uint8_t err = i2cProbe(kAddr[i]);
    if (err != 0) {
      Serial.printf("ADS: 0x%02X not found (I2C err=%u %s)\n",
                    kAddr[i], (unsigned)err, voltageI2cErrLabel(err));
      s_ok[i] = 0;
      s_err[i] = err;
      continue;
    }
    if (!adsInitChip(i)) {
      Serial.printf("ADS: 0x%02X begin() failed\n", kAddr[i]);
      s_ok[i] = 0;
      s_err[i] = err;
      continue;
    }
    s_ok[i] = 1;
    s_err[i] = 0;
    Serial.printf("ADS: 0x%02X ok GAIN_ONE 128SPS\n", kAddr[i]);
  }

  if (!s_ok[0] && !s_ok[1]) {
    Serial.println("ADS: none detected — DC motors cannot start");
  }
}

void voltageReprobe() {
  i2cInitOnce();
  i2cScan();
  for (int i = 0; i < 2; i++) {
    const uint8_t err = i2cProbe(kAddr[i]);
    const bool now_ok = (err == 0);
    if (now_ok && !s_ok[i]) {
      if (adsInitChip(i)) {
        s_ok[i] = 1;
        s_err[i] = 0;
        Serial.printf("ADS: 0x%02X re-probed ok\n", kAddr[i]);
      } else {
        s_ok[i] = 0;
        s_err[i] = err;
        Serial.printf("ADS: 0x%02X begin() failed on re-probe\n", kAddr[i]);
      }
    } else if (!now_ok && s_ok[i]) {
      s_ok[i] = 0;
      s_err[i] = err;
      Serial.printf("ADS: 0x%02X lost (I2C err=%u %s)\n",
                    kAddr[i], (unsigned)err, voltageI2cErrLabel(err));
    } else if (!now_ok) {
      s_err[i] = err;
    }
  }
}

bool voltageAdsOk(int chip) {
  return chip >= 0 && chip < 2 && s_ok[chip];
}

uint8_t voltageAdsErr(int chip) {
  return (chip >= 0 && chip < 2) ? s_err[chip] : 0;
}

static int chipOf(int ch) {
  return (ch < 4) ? 1 : 0;
}

static int ainOf(int ch) {
  return ch & 3;
}

static bool readAdcVolts(int chip, int ain, uint8_t logical_channel,
                         float *out_v, int16_t *out_raw) {
  if (!s_ok[chip] || ain < 0 || ain > 3 || !out_v) {
    return false;
  }
  const uint32_t t0 = millis();
  i2cLock();
  s_ads[chip].startADCReading(MUX_BY_CHANNEL[ain], false);
  i2cUnlock();

  // At 128 SPS the conversion period is 7.8 ms. Let the ADS complete
  // without holding the shared bus mutex; the display can use the bus here.
  vTaskDelay(pdMS_TO_TICKS(10));
  for (;;) {
    i2cLock();
    const bool done = s_ads[chip].conversionComplete();
    i2cUnlock();
    if (done) {
      break;
    }
    if ((uint32_t)(millis() - t0) > (uint32_t)MPS_ADS_READ_TIMEOUT_MS) {
      s_voltage_timeout_count[chip]++;
      recordVoltageRead(logical_channel, chip, ain, false, 0, 0);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  i2cLock();
  const int16_t raw = s_ads[chip].getLastConversionResults();
  i2cUnlock();
  *out_v = s_ads[chip].computeVolts(raw);
  if (out_raw) {
    *out_raw = raw;
  }
  recordVoltageRead(logical_channel, chip, ain, true, raw, *out_v);
  return true;
}

void voltageCalibrateAll(ChannelRuntime *ch) {
  i2cInitOnce();
  for (int i = 0; i < MAX_CHANNELS; i++) {
    ch[i].last_v = 0;
    ch[i].v_calibrated = 0;
    ch[i].v_zero = 0;
    ch[i].v_filt = 0;
    ch[i].v_filt_valid = 0;
    const int chip = chipOf(i);
    if (!s_ok[chip]) {
      continue;
    }
    double acc = 0;
    int n = 0;
    for (int k = 0; k < V_CAL_SAMPLES; k++) {
      float v = 0;
      if (!readAdcVolts(chip, ainOf(i), (uint8_t)i, &v, nullptr)) {
        continue;
      }
      acc += (double)v;
      n++;
    }
    if (n > 0) {
      ch[i].v_zero = (float)(acc / (double)n);
      ch[i].v_calibrated = 1;
      Serial.printf("CAL: CH%d v_zero=%.4f V (%d samples)\n",
                    i, (double)ch[i].v_zero, n);
    } else {
      Serial.printf("CAL: CH%d voltage zero failed\n", i);
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
  double acc = 0;
  int n = 0;
  uint8_t bad = 0;
  VoltageDiagnostic diag;
  memset(&diag, 0, sizeof(diag));
  diag.timestamp_ms = millis();
  diag.logical_channel = (uint8_t)ch;
  diag.chip = (uint8_t)chip;
  diag.address = kAddr[chip];
  diag.ain = (uint8_t)ainOf(ch);
  diag.v_zero = rt->v_zero;
  diag.sample_count = V_AVG_SAMPLES;
  const uint32_t timeout_before = s_voltage_timeout_count[chip];
  for (int k = 0; k < V_AVG_SAMPLES; k++) {
    float vadc = 0;
    int16_t raw = 0;
    if (!readAdcVolts(chip, ainOf(ch), (uint8_t)ch, &vadc, &raw)) {
      if (k < VOLTAGE_DIAG_SAMPLES) {
        diag.sample_valid[k] = 0;
      }
      continue;
    }
    if (k < VOLTAGE_DIAG_SAMPLES) {
      diag.raw[k] = raw;
      diag.sample_v_adc[k] = vadc;
      diag.sample_valid[k] = 1;
    }
    acc += (double)vadc;
    n++;
    diag.successful_reads++;
    if (vadc > VADC_ABS_MAX || vadc < -0.05f) {
      bad = 1;
    }
  }
  if (n == 0) {
    diag.fault = 1;
    diag.timeout_count = s_voltage_timeout_count[chip] - timeout_before;
    s_voltage_diag[ch] = diag;
    return r;
  }
  r.present = 1;
  r.v_adc = (float)(acc / (double)n);
  r.v_bus = (r.v_adc - rt->v_zero) * VDIV_SCALE;
  r.fault = 0;
  if (bad || r.v_adc > VADC_ABS_MAX || r.v_adc < -0.05f || fabsf(r.v_bus) > VBUS_CAP) {
    r.fault = 1;
  }
  diag.v_adc = r.v_adc;
  diag.v_bus = r.v_bus;
  diag.fault = r.fault;
  diag.timeout_count = s_voltage_timeout_count[chip] - timeout_before;
  s_voltage_diag[ch] = diag;
  return r;
}

bool voltageDiagnosticGet(int ch, VoltageDiagnostic *out) {
  if (ch < 0 || ch >= MAX_CHANNELS || !out) {
    return false;
  }
  *out = s_voltage_diag[ch];
  return true;
}

uint8_t voltageDiagnosticCopyHistory(int ch, VoltageReadDiagnostic *out,
                                     uint8_t capacity) {
  if (ch < 0 || ch >= MAX_CHANNELS || !out || capacity == 0) {
    return 0;
  }
  const uint8_t count = s_voltage_ring_count[ch];
  const uint8_t n = count < capacity ? count : capacity;
  const uint8_t start =
      (uint8_t)((s_voltage_ring_head[ch] + VOLTAGE_DIAG_RING - n) %
                VOLTAGE_DIAG_RING);
  for (uint8_t i = 0; i < n; i++) {
    out[i] = s_voltage_ring[ch][(start + i) % VOLTAGE_DIAG_RING];
  }
  return n;
}

uint32_t voltageDiagnosticTimeouts(int chip) {
  return (chip >= 0 && chip < 2) ? s_voltage_timeout_count[chip] : 0;
}
