#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_ADS1X15.h>
#include "voltage.h"
#include "config_pins.h"
#include "config_limits.h"

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static Adafruit_ADS1115 s_ads[2];
static uint8_t s_ok[2] = {0, 0};
static uint8_t s_i2c_initialized = 0;
static const uint8_t kAddr[2] = {ADS1115_ADDR_A, ADS1115_ADDR_B};

// ---------------------------------------------------------------------------
// I2C helpers — no Wire.end() / repeated Wire.begin() cycles
// ---------------------------------------------------------------------------

// Initialize the I2C bus once with the correct pins and clock speed.
// Subsequent calls are no-ops if already initialized.
static void i2cInitOnce() {
    if (!s_i2c_initialized) {
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        Wire.setClock(400000);
        s_i2c_initialized = 1;
        Serial.printf("I2C: initialized SDA=%d SCL=%d clock=400kHz\n",
                      PIN_I2C_SDA, PIN_I2C_SCL);
    }
}

// Probe a single I2C address. Returns 0 if a device ACKs, non-zero on error.
static uint8_t i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission();
}

// Scan the I2C bus and print all responding addresses (0x03..0x77).
// Useful for diagnosing wiring / address configuration issues.
static void i2cScan() {
    Serial.println("I2C bus scan:");
    Serial.println("-------------");
    uint8_t found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        const uint8_t err = i2cProbe(addr);
        if (err == 0) {
            Serial.printf("  0x%02X  <--- device found\n", addr);
            found++;
        }
    }
    if (found == 0) {
        Serial.println("  (no devices found — check wiring/pull-ups/power)");
    }
    Serial.printf("Expected: 0x%02X (ADDR->GND)=CH0-3, 0x%02X (ADDR->VDD)=CH4-7\n",
                  ADS1115_ADDR_A, ADS1115_ADDR_B);
    Serial.printf("Scan complete: %u device(s) found\n", found);
}

// ---------------------------------------------------------------------------
// ADS1115 initialization (safe — no Wire.end() cycles)
// ---------------------------------------------------------------------------

void voltageBegin() {
    s_ok[0] = 0;
    s_ok[1] = 0;

    // Initialize I2C once
    i2cInitOnce();

    // Diagnostic scan
    i2cScan();

    // Initialize each ADS1115
    for (int i = 0; i < 2; i++) {
        Serial.printf("ADS1115 0x%02X: probing... ", kAddr[i]);

        // Quick probe before Adafruit library init
        const uint8_t probe_err = i2cProbe(kAddr[i]);
        if (probe_err != 0) {
            Serial.printf("NOT FOUND (I2C err=%u)\n", probe_err);
            s_ok[i] = 0;
            continue;
        }

        // Initialize with Adafruit library
        const bool begun = s_ads[i].begin(kAddr[i], &Wire);
        if (!begun) {
            Serial.println("begin() FAILED");
            s_ok[i] = 0;
            continue;
        }

        // Configure
        s_ads[i].setGain(GAIN_ONE);
        s_ads[i].setDataRate(RATE_ADS1115_250SPS);
        s_ok[i] = 1;
        Serial.println("OK (GAIN_ONE, 250SPS)");
    }

    // Summary
    Serial.printf("ADS1115 summary: 0x%02X=%s, 0x%02X=%s\n",
                  ADS1115_ADDR_A, s_ok[0] ? "OK" : "FAIL",
                  ADS1115_ADDR_B, s_ok[1] ? "OK" : "FAIL");

    if (!s_ok[0] && !s_ok[1]) {
        Serial.println("WARNING: No ADS1115 detected. DC motors cannot start.");
        Serial.println("Check: ADDR pin config, pull-up resistors, wiring, power.");
    }
}

void voltageReprobe() {
    // Re-probe without full re-initialization (for Calibrate command)
    i2cInitOnce();  // Ensure bus is up

    for (int i = 0; i < 2; i++) {
        const uint8_t err = i2cProbe(kAddr[i]);
        const bool now_ok = (err == 0);

        if (now_ok && !s_ok[i]) {
            // Chip appeared — initialize it
            const bool begun = s_ads[i].begin(kAddr[i], &Wire);
            if (begun) {
                s_ads[i].setGain(GAIN_ONE);
                s_ads[i].setDataRate(RATE_ADS1115_250SPS);
                s_ok[i] = 1;
                Serial.printf("ADS1115 0x%02X: re-probed OK\n", kAddr[i]);
            }
        } else if (!now_ok && s_ok[i]) {
            // Chip disappeared
            s_ok[i] = 0;
            Serial.printf("ADS1115 0x%02X: lost (I2C err=%u)\n", kAddr[i], err);
        }
        // else: no change
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool voltageAdsOk(int chip) {
    return chip >= 0 && chip < 2 && s_ok[chip];
}

// ---------------------------------------------------------------------------
// Channel mapping (fixed — matches firmware expectations)
// ---------------------------------------------------------------------------

static int chipOf(int ch) {
    // CH0-3 -> chip 0 (0x48), CH4-7 -> chip 1 (0x49)
    return (ch < 4) ? 0 : 1;
}

static int ainOf(int ch) {
    // AIN0-3 on each chip
    return ch & 3;
}

// ---------------------------------------------------------------------------
// ADC reading with timeout (no hang)
// ---------------------------------------------------------------------------

static bool readAdcVolts(int chip, int ain, float *out_v) {
    if (!s_ok[chip] || ain < 0 || ain > 3 || !out_v) {
        return false;
    }

    // Start a single-shot conversion
    s_ads[chip].startADCReading(MUX_BY_CHANNEL[ain], false);

    // Wait with timeout (20 ms max — 250SPS = 4ms nominal)
    const uint32_t t0 = millis();
    while (!s_ads[chip].conversionComplete()) {
        if ((uint32_t)(millis() - t0) > 20) {
            return false;  // Timeout — chip unresponsive
        }
        delay(1);
    }

    // Read result
    const int16_t raw = s_ads[chip].getLastConversionResults();
    *out_v = s_ads[chip].computeVolts(raw);
    return true;
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

void voltageCalibrateAll(ChannelRuntime *ch) {
    // Ensure I2C is up
    i2cInitOnce();

    Serial.println("Voltage calibration:");
    Serial.println("--------------------");

    for (int i = 0; i < MAX_CHANNELS; i++) {
        ch[i].last_v = 0;
        ch[i].v_calibrated = 0;
        ch[i].v_zero = 0;

        const int chip = chipOf(i);
        if (!s_ok[chip]) {
            continue;  // Chip not present — skip this channel
        }

        double acc = 0;
        int n = 0;

        // Average multiple samples for a stable zero
        for (int k = 0; k < V_CAL_SAMPLES; k++) {
            float v = 0;
            if (!readAdcVolts(chip, ainOf(i), &v)) {
                continue;  // Read failed — skip this sample
            }
            acc += (double)v;
            n++;
        }

        if (n > 0) {
            ch[i].v_zero = (float)(acc / (double)n);
            ch[i].v_calibrated = 1;
            Serial.printf("  CH%d: v_zero=%.4f V (%d samples)\n",
                         i, (double)ch[i].v_zero, n);
        } else {
            Serial.printf("  CH%d: calibration FAILED (no valid samples)\n", i);
        }
    }

    Serial.println("Voltage calibration complete.");
}

// ---------------------------------------------------------------------------
// Runtime sampling
// ---------------------------------------------------------------------------

VoltageSample voltageSample(int ch, const ChannelRuntime *rt) {
    VoltageSample r;
    r.v_bus = 0;
    r.v_adc = 0;
    r.present = 0;
    r.fault = 1;

    // Validate inputs
    if (ch < 0 || ch >= MAX_CHANNELS || !rt) {
        return r;
    }

    const int chip = chipOf(ch);
    if (!s_ok[chip]) {
        return r;  // Chip not present
    }

    // Read the ADC
    float vadc = 0;
    if (!readAdcVolts(chip, ainOf(ch), &vadc)) {
        return r;  // Read failed (timeout or error)
    }

    // Populate result
    r.present = 1;
    r.v_adc = vadc;

    // Apply zero offset and scale
    r.v_bus = (r.v_adc - rt->v_zero) * VDIV_SCALE;

    // Fault checks
    r.fault = 0;
    if (r.v_adc > VADC_ABS_MAX) {
        r.fault = 1;  // ADC input above safe range
    }
    if (r.v_adc < -0.05f) {
        r.fault = 1;  // Negative (below ground)
    }
    if (fabsf(r.v_bus) > VBUS_CAP) {
        r.fault = 1;  // Bus voltage above maximum
    }

    return r;
}

// ---------------------------------------------------------------------------
// Diagnostics helper (called from protection task)
// ---------------------------------------------------------------------------

void protectionCopyVcal(uint8_t out[MAX_CHANNELS]) {
    if (!out) {
        return;
    }
    // This is a stub — the actual implementation is in protection.cpp
    // This function exists to maintain API compatibility
    // The protection task reads v_calibrated directly from ChannelRuntime
    (void)out;
}
