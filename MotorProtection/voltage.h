#pragma once

#include <stdint.h>
#include "config_limits.h"

// Voltage sample result for one channel.
struct VoltageSample {
    float v_bus;      // Scaled bus voltage (V) after zero subtraction
    float v_adc;      // Raw ADC voltage at the ADS1115 input (V)
    uint8_t present;  // 1 if the ADS1115 chip responded
    uint8_t fault;    // 1 if out of range or read failed
};

// Initialize I2C and probe both ADS1115 modules.
// Call once at boot. Prints diagnostics to Serial.
void voltageBegin();

// Re-probe ADS1115 modules (called on Calibrate command).
// Safe to call at runtime. Does not hang if chips are missing.
void voltageReprobe();

// Returns 1 if the ADS1115 chip for the given index (0 or 1) is responsive.
bool voltageAdsOk(int chip);

// Calibrate DC voltage zeros for all channels.
// Assumes relays are open (0 V at all voltage taps).
void voltageCalibrateAll(ChannelRuntime *ch);

// Sample one channel's DC voltage. Returns present=0 if the chip is missing.
VoltageSample voltageSample(int ch, const ChannelRuntime *rt);

// Copy which channels have valid voltage calibration (1) or not (0).
void protectionCopyVcal(uint8_t out[MAX_CHANNELS]);
