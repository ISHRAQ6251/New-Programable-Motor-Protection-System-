#pragma once

#include <stdint.h>
#include "types.h"

#ifndef MPS_I2C_HZ
#define MPS_I2C_HZ 400000
#endif
#ifndef MPS_ADS_READ_TIMEOUT_MS
#define MPS_ADS_READ_TIMEOUT_MS 20
#endif

struct VoltageSample {
  float v_bus;
  float v_adc;
  uint8_t present;
  uint8_t fault;
};

void voltageI2cMutexInit();
void voltageBegin();
void voltageReprobe();
bool voltageAdsOk(int chip);
uint8_t voltageAdsErr(int chip);
const char *voltageI2cErrLabel(uint8_t e);
void voltageCalibrateAll(ChannelRuntime *ch);
VoltageSample voltageSample(int ch, const ChannelRuntime *rt);

// Shared I2C bus mutex (ADS1115 in this file, OLED in ui.cpp). Hold only
// across the actual Wire/library I2C call — never across delay() or drawing.
void i2cLock();
void i2cUnlock();
