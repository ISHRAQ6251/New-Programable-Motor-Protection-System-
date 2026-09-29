#pragma once

#include <stdint.h>
#include "types.h"

struct SampleResult {
  float rms;
  float mean_v;
  uint8_t stuck;
  uint8_t out_of_range;
  double sum_i2;
  int n;
};

void sensingBegin();
void sensingCalibrateAll(ChannelRuntime *ch);
void sensingResetAcWindow(ChannelRuntime *ch);
float sensingPushAcCycle(ChannelRuntime *ch, double sum_i2, int n);
SampleResult sensingSample(int ch, const ChannelRuntime *rt, uint8_t is_ac, uint8_t mains_hz);
void sensingSampleAcChannels(const uint8_t *ch_list, int nch, const ChannelRuntime *rt,
                             uint8_t mains_hz, SampleResult *out_by_ch);
float sensingAdcToAmps(int millivolts, float zero_mv);
