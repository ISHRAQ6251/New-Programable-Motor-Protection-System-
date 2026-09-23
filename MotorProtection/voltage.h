#pragma once

#include <stdint.h>
#include "types.h"

#ifndef MPS_I2C_HZ
#define MPS_I2C_HZ 400000
#endif
#ifndef MPS_ADS_READ_TIMEOUT_MS
#define MPS_ADS_READ_TIMEOUT_MS 35
#endif

struct VoltageSample {
  float v_bus;
  float v_adc;
  uint8_t present;
  uint8_t fault;
};

static const uint8_t VOLTAGE_DIAG_SAMPLES = 4;
static const uint8_t VOLTAGE_DIAG_RING = 16;

struct VoltageReadDiagnostic {
  uint32_t timestamp_ms;
  uint8_t logical_channel;
  uint8_t chip;
  uint8_t address;
  uint8_t ain;
  uint8_t valid;
  int16_t raw;
  float v_adc;
};

struct VoltageDiagnostic {
  uint32_t timestamp_ms;
  uint8_t logical_channel;
  uint8_t chip;
  uint8_t address;
  uint8_t ain;
  uint8_t sample_count;
  uint8_t successful_reads;
  uint8_t fault;
  uint32_t timeout_count;
  float v_zero;
  float v_adc;
  float v_bus;
  int16_t raw[VOLTAGE_DIAG_SAMPLES];
  float sample_v_adc[VOLTAGE_DIAG_SAMPLES];
  uint8_t sample_valid[VOLTAGE_DIAG_SAMPLES];
};

void voltageI2cMutexInit();
void voltageBegin();
void voltageReprobe();
bool voltageAdsOk(int chip);
uint8_t voltageAdsErr(int chip);
const char *voltageI2cErrLabel(uint8_t e);
void voltageCalibrateAll(ChannelRuntime *ch);
VoltageSample voltageSample(int ch, const ChannelRuntime *rt);
bool voltageDiagnosticGet(int ch, VoltageDiagnostic *out);
uint8_t voltageDiagnosticCopyHistory(int ch, VoltageReadDiagnostic *out,
                                     uint8_t capacity);
uint32_t voltageDiagnosticTimeouts(int chip);

// Shared I2C bus mutex (ADS1115 in this file, OLED in ui.cpp). Hold only
// across the actual Wire/library I2C call — never across delay() or drawing.
void i2cLock();
void i2cUnlock();
