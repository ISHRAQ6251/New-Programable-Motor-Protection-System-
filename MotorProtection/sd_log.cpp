#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "sd_log.h"
#include "config_pins.h"
#include "config_limits.h"

static bool s_ok = false;

static const char *faultName(FaultType t) {
  switch (t) {
    case FT_I2T:       return "I2T";
    case FT_STALL:     return "STALL";
    case FT_SENSOR:    return "SENSOR_FAULT";
    case FT_UNDERVOLT: return "UNDERVOLT";
    case FT_OVERVOLT:  return "OVERVOLT";
    case FT_NO_CURRENT: return "NO_CURRENT";
    default:           return "UNKNOWN";
  }
}

void sdLogBegin() {
  s_ok = false;
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, 4000000)) {
    Serial.println("SD: mount failed — card missing, 5 V on a 3.3 V breakout, or CS/SPI wiring");
    Serial.println("SD: fault log unavailable; protection still runs");
    return;
  }
  if (!SD.exists(FAULT_LOG_PATH)) {
    File f = SD.open(FAULT_LOG_PATH, FILE_WRITE);
    if (!f) {
      Serial.println("SD: card mounted but cannot create /faults.csv (read-only or full?)");
      return;
    }
    f.println("uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA");
    f.close();
  }
  s_ok = true;
  Serial.println("SD: mounted, log /faults.csv");
}

bool sdLogOk() {
  return s_ok;
}

void sdLogAppend(const LogEvent *ev) {
  if (!s_ok || !ev) {
    return;
  }
  File f = SD.open(FAULT_LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("SD: append failed — card removed or filesystem error");
    s_ok = false;
    return;
  }
  char pwr_w[16];
  char pwr_va[16];
  pwr_w[0] = 0;
  pwr_va[0] = 0;
  if (ev->power_is_w) {
    snprintf(pwr_w, sizeof(pwr_w), "%.1f", (double)ev->power);
  } else {
    snprintf(pwr_va, sizeof(pwr_va), "%.0f", (double)ev->power);
  }
  char line[160];
  snprintf(line, sizeof(line), "%lu,%s,%s,%.3f,%.3f,%s,%s",
           (unsigned long)ev->uptime_ms,
           ev->motor,
           faultName(ev->type),
           (double)ev->current_a,
           (double)ev->voltage_v,
           pwr_w,
           pwr_va);
  f.println(line);
  f.close();
}

bool sdLogClear() {
  if (!s_ok) {
    return false;
  }
  SD.remove(FAULT_LOG_PATH);
  File f = SD.open(FAULT_LOG_PATH, FILE_WRITE);
  if (!f) {
    s_ok = false;
    return false;
  }
  f.println("uptime_ms,motor,type,current_A,voltage_V,power_W,power_VA");
  f.close();
  return true;
}

bool sdLogReadAll(char *buf, size_t buf_len, size_t *out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (!s_ok || !buf || buf_len < 2) {
    return false;
  }
  File f = SD.open(FAULT_LOG_PATH, FILE_READ);
  if (!f) {
    return false;
  }
  size_t n = f.readBytes(buf, buf_len - 1);
  f.close();
  buf[n] = 0;
  if (out_len) {
    *out_len = n;
  }
  return true;
}
