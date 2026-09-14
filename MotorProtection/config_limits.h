#pragma once

#include <stdint.h>

static const int MAX_CHANNELS = 8;
static const int MAX_MOTORS   = 8;
static const int MAX_STEPS    = 8;
static const int MAX_PHASES   = 3;

static const int NAME_LEN      = 24;
static const int AUTH_USER_LEN = 32;
static const int AUTH_PASS_LEN = 32;

static const uint8_t CH_UNUSED = 0xFF;

static const int RMS_SAMPLES     = 32;
static const int CAL_SAMPLES     = 64;
static const float SENSOR_I_CAP  = 40.0f;
static const float VADC_MIN      = 0.05f;
static const float VADC_MAX      = 3.05f;

static const int V_CAL_SAMPLES     = 16;
static const float VBUS_CAP        = 55.0f;
static const float VADC_ABS_MAX    = 4.0f;
static const uint32_t UV_GRACE_MS  = 250;

static const uint32_t NVS_MAGIC   = 0x4D505331u;
static const uint16_t NVS_SCHEMA  = 2;

static const char *NVS_NS        = "mps";
static const char *NVS_KEY_BLOB  = "motors";
static const char *NVS_KEY_USER  = "auth_user";
static const char *NVS_KEY_PASS  = "auth_pass";

static const char *DEFAULT_AUTH_USER = "mps";
static const char *DEFAULT_AUTH_PASS = "mps500";

static const char *AP_SSID = "MPS-505";
static const char *AP_PASS = "mps50005";

static const char *FAULT_LOG_PATH = "/faults.csv";
