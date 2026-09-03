#pragma once

#include <stdint.h>
#include "types.h"

struct SampleResult {
  float rms;
  float mean_v;
  uint8_t stuck;
  uint8_t out_of_range;
};

void sensingBegin();
void sensingCalibrateAll(ChannelRuntime *ch);
SampleResult sensingSample(int ch, const ChannelRuntime *rt, uint8_t is_ac, uint8_t mains_hz);
float sensingAdcToAmps(int millivolts, float zero_mv);
