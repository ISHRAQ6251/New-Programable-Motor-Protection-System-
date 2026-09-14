#pragma once

#include <stdint.h>
#include "types.h"

struct VoltageSample {
  float v_bus;
  float v_adc;
  uint8_t present;
  uint8_t fault;
};

void voltageBegin();
void voltageRebindWire();
bool voltageAdsOk(int chip);
void voltageCalibrateAll(ChannelRuntime *ch);
VoltageSample voltageSample(int ch, const ChannelRuntime *rt);
