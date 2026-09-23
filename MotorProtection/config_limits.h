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
static const uint8_t VCH_SAME  = 0xFF;
static const uint8_t RELAY_ACTIVE_HIGH_DEFAULT = 0;

static const int RMS_SAMPLES     = 32;
static const int CAL_SAMPLES     = 64;
static const float SENSOR_I_CAP  = 40.0f;
static const float VADC_MIN      = 0.05f;
static const float VADC_MAX      = 3.05f;

static const int V_CAL_SAMPLES     = 16;
static const int V_AVG_SAMPLES     = 4;
static const float VBUS_CAP        = 55.0f;
static const float VADC_ABS_MAX    = 4.0f;
static const uint32_t UV_GRACE_MS  = 750;
static const uint32_t VOLT_LOG_MS  = 2000;

static const float NO_CURRENT_FRAC  = 0.05f;
static const uint32_t NO_CURRENT_MS = 2000u;
static const uint8_t SENSOR_FAULT_MIN_COUNT = 3;
static const uint32_t SENSOR_FAULT_WINDOW_MS = 1000u;
static const uint8_t MAX_RESTARTS   = 3;
static const uint32_t CLEAN_RUN_MS  = 600000u;
static const float COOLING_MAX_S    = 86400.0f;
static const uint32_t WDT_TIMEOUT_MS = 5000u;
static const float DT_MAX_S         = 5.0f;

static const uint32_t ICD_MIN_MS = 0u;
static const uint32_t ICD_MAX_MS = 10000u;
static const float ICD_MIN_CURRENT_FACTOR = 1.0f;

static const uint8_t JAM_RELEASE_MAX        = 4;
static const uint32_t JAM_RELEASE_OFF_MS    = 300;
static const uint32_t JAM_RELEASE_WAIT_MS   = 500;
static const uint32_t JAM_RELEASE_MIN_RUN_MS = 500;

static const int LOG_RING = 32;

static const uint32_t NVS_MAGIC   = 0x4D505331u;
static const uint16_t NVS_SCHEMA  = 4;

static const char *NVS_NS        = "mps";
static const char *NVS_KEY_BLOB  = "motors";
static const char *NVS_KEY_USER  = "auth_user";
static const char *NVS_KEY_PASS  = "auth_pass";

static const char *DEFAULT_AUTH_USER = "mps";
static const char *DEFAULT_AUTH_PASS = "mps500";

static const char *AP_SSID = "MPS-505";
static const char *AP_PASS = "mps50005";

static const char *FAULT_LOG_PATH = "/faults.csv";
