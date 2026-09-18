#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "motor_store.h"
#include "config_limits.h"

static Preferences s_prefs;
static MotorBlob s_blob;
static char s_user[AUTH_USER_LEN];
static char s_pass[AUTH_PASS_LEN];
static SemaphoreHandle_t s_mu;

static void lock() {
  if (s_mu) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
  }
}

static void unlock() {
  if (s_mu) {
    xSemaphoreGive(s_mu);
  }
}

static void clearMotors() {
  memset(&s_blob, 0, sizeof(s_blob));
  s_blob.magic = NVS_MAGIC;
  s_blob.schema_version = NVS_SCHEMA;
  for (int i = 0; i < MAX_MOTORS; i++) {
    for (int p = 0; p < MAX_PHASES; p++) {
      s_blob.motors[i].channels[p] = CH_UNUSED;
      s_blob.motors[i].relay_active_high[p] = 1;
    }
  }
}

static void clearRecord(MotorRecord *m) {
  memset(m, 0, sizeof(*m));
  for (int p = 0; p < MAX_PHASES; p++) {
    m->channels[p] = CH_UNUSED;
    m->relay_active_high[p] = 1;
  }
}

static bool channelTaken(uint8_t ch, int skip_idx) {
  for (int i = 0; i < MAX_MOTORS; i++) {
    if (i == skip_idx) {
      continue;
    }
    const MotorRecord *m = &s_blob.motors[i];
    if (!m->used) {
      continue;
    }
    for (int p = 0; p < m->phase_count; p++) {
      if (m->channels[p] == ch) {
        return true;
      }
    }
  }
  return false;
}

static bool validateStructural(const MotorRecord *m, char *err, size_t err_len) {
  if (!m->name[0]) {
    snprintf(err, err_len, "name required");
    return false;
  }
  for (int i = 0; m->name[i]; i++) {
    const char c = m->name[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                    c == '-' || c == '.';
    if (!ok) {
      snprintf(err, err_len, "name: letters, digits, space, _ - . only");
      return false;
    }
  }
  if (m->phase_count != 1 && m->phase_count != 3) {
    snprintf(err, err_len, "phase_count must be 1 or 3");
    return false;
  }
  if (!(m->in_amps > 0.0f) || !isfinite(m->in_amps)) {
    snprintf(err, err_len, "operating current must be > 0");
    return false;
  }
  if (!(m->stall_amps > 0.0f) || !isfinite(m->stall_amps)) {
    snprintf(err, err_len, "stall current must be > 0");
    return false;
  }
  if (!(m->cooling_s > 0.0f) || !isfinite(m->cooling_s)) {
    snprintf(err, err_len, "cooling time must be > 0");
    return false;
  }
  if (m->is_ac) {
    if (m->mains_hz != 50 && m->mains_hz != 60) {
      snprintf(err, err_len, "mains_hz must be 50 or 60");
      return false;
    }
    if (!(m->rated_ac_v > 0.0f) || !isfinite(m->rated_ac_v) || m->rated_ac_v > 1000.0f) {
      snprintf(err, err_len, "rated AC voltage must be 0.1..1000");
      return false;
    }
  } else {
    if (!(m->uv_volts >= 0.0f) || !isfinite(m->uv_volts) ||
        !(m->ov_volts >= 0.0f) || !isfinite(m->ov_volts)) {
      snprintf(err, err_len, "UV/OV cannot be negative");
      return false;
    }
    if (m->uv_volts > 0.0f && m->ov_volts > 0.0f && m->ov_volts <= m->uv_volts) {
      snprintf(err, err_len, "overvoltage must be greater than undervoltage");
      return false;
    }
    if (m->uv_volts > VBUS_CAP || m->ov_volts > VBUS_CAP) {
      snprintf(err, err_len, "UV/OV must be at most 55 V");
      return false;
    }
  }
  if (m->step_count < 1 || m->step_count > MAX_STEPS) {
    snprintf(err, err_len, "step_count must be 1..8");
    return false;
  }
  for (int i = 0; i < m->step_count; i++) {
    if (!(m->step_k[i] > 0.0f) || !(m->step_t_s[i] > 0.0f) ||
        !isfinite(m->step_k[i]) || !isfinite(m->step_t_s[i])) {
      snprintf(err, err_len, "each step needs k > 0 and t > 0");
      return false;
    }
  }
  return true;
}

static bool validateRecord(const MotorRecord *m, char *err, size_t err_len) {
  if (!validateStructural(m, err, err_len)) {
    return false;
  }
  if (m->cooling_s > COOLING_MAX_S) {
    snprintf(err, err_len, "cooling time must be at most 86400 s");
    return false;
  }
  float max_curve_a = 0.0f;
  for (int i = 0; i < m->step_count; i++) {
    const float a = m->step_k[i] * m->in_amps;
    if (a > max_curve_a) {
      max_curve_a = a;
    }
  }
  if (m->stall_amps <= m->in_amps || m->stall_amps <= max_curve_a) {
    snprintf(err, err_len, "stall current must exceed In and every I2t step");
    return false;
  }
  return true;
}

static void sanitizeLoadedBlob() {
  bool used_ch[MAX_CHANNELS];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    used_ch[i] = false;
  }
  for (int i = 0; i < MAX_MOTORS; i++) {
    MotorRecord *m = &s_blob.motors[i];
    bool ok = (m->used != 0);
    if (ok) {
      m->used = 1;
      m->name[NAME_LEN - 1] = 0;
      char err[48];
      if (!validateStructural(m, err, sizeof(err))) {
        ok = false;
      } else {
        for (int p = 0; p < m->phase_count; p++) {
          const uint8_t c = m->channels[p];
          if (c >= MAX_CHANNELS || used_ch[c]) {
            ok = false;
            break;
          }
          for (int q = 0; q < p; q++) {
            if (m->channels[q] == c) {
              ok = false;
              break;
            }
          }
          if (!ok) {
            break;
          }
        }
        if (ok) {
          for (int p = 0; p < m->phase_count; p++) {
            used_ch[m->channels[p]] = true;
          }
        }
      }
    }
    if (!ok) {
      if (m->used) {
        Serial.printf("NVS: motor %d invalid — dropped\n", i);
      }
      clearRecord(m);
    }
  }
}

void motorStoreBegin() {
  s_mu = xSemaphoreCreateMutex();
  strncpy(s_user, DEFAULT_AUTH_USER, AUTH_USER_LEN - 1);
  strncpy(s_pass, DEFAULT_AUTH_PASS, AUTH_PASS_LEN - 1);
  s_user[AUTH_USER_LEN - 1] = 0;
  s_pass[AUTH_PASS_LEN - 1] = 0;
  clearMotors();
  if (!s_prefs.begin(NVS_NS, false)) {
    Serial.println("NVS: begin failed — empty motor list");
    return;
  }
  motorStoreLoad();
}

void motorStoreLoad() {
  lock();
  if (s_prefs.getString(NVS_KEY_USER, s_user, AUTH_USER_LEN) == 0) {
    strncpy(s_user, DEFAULT_AUTH_USER, AUTH_USER_LEN - 1);
    s_user[AUTH_USER_LEN - 1] = 0;
  }
  if (s_prefs.getString(NVS_KEY_PASS, s_pass, AUTH_PASS_LEN) == 0) {
    strncpy(s_pass, DEFAULT_AUTH_PASS, AUTH_PASS_LEN - 1);
    s_pass[AUTH_PASS_LEN - 1] = 0;
  }

  const size_t n = s_prefs.getBytesLength(NVS_KEY_BLOB);
  if (n != sizeof(MotorBlob)) {
    Serial.println("NVS: no motor blob — starting empty");
    clearMotors();
    unlock();
    return;
  }
  MotorBlob tmp;
  if (s_prefs.getBytes(NVS_KEY_BLOB, &tmp, sizeof(tmp)) != sizeof(tmp)) {
    Serial.println("NVS: read failed — empty motor list");
    clearMotors();
    unlock();
    return;
  }
  if (tmp.magic != NVS_MAGIC || tmp.schema_version != NVS_SCHEMA) {
    Serial.printf("NVS: schema %u (need %u) — starting empty\n",
                  (unsigned)tmp.schema_version, (unsigned)NVS_SCHEMA);
    clearMotors();
    unlock();
    return;
  }
  s_blob = tmp;
  sanitizeLoadedBlob();
  unlock();
}

bool motorStoreSave() {
  lock();
  s_blob.magic = NVS_MAGIC;
  s_blob.schema_version = NVS_SCHEMA;
  const size_t w = s_prefs.putBytes(NVS_KEY_BLOB, &s_blob, sizeof(s_blob));
  unlock();
  if (w != sizeof(s_blob)) {
    Serial.println("NVS: save failed");
    return false;
  }
  return true;
}

void motorStoreGet(MotorRecord *out) {
  lock();
  memcpy(out, s_blob.motors, sizeof(s_blob.motors));
  unlock();
}

void motorStoreSet(const MotorRecord *in) {
  lock();
  memcpy(s_blob.motors, in, sizeof(s_blob.motors));
  unlock();
}

int motorStoreFreeCount() {
  lock();
  bool used[MAX_CHANNELS] = {false};
  for (int i = 0; i < MAX_MOTORS; i++) {
    const MotorRecord *m = &s_blob.motors[i];
    if (!m->used) {
      continue;
    }
    for (int p = 0; p < m->phase_count; p++) {
      if (m->channels[p] < MAX_CHANNELS) {
        used[m->channels[p]] = true;
      }
    }
  }
  int n = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!used[i]) {
      n++;
    }
  }
  unlock();
  return n;
}

static bool allocLocked(uint8_t phase_count, uint8_t *out_ch) {
  for (int p = 0; p < MAX_PHASES; p++) {
    out_ch[p] = CH_UNUSED;
  }
  int found = 0;
  for (int c = 0; c < MAX_CHANNELS && found < phase_count; c++) {
    if (!channelTaken((uint8_t)c, -1)) {
      out_ch[found++] = (uint8_t)c;
    }
  }
  return found == phase_count;
}

bool motorStoreAlloc(uint8_t phase_count, uint8_t *out_ch) {
  lock();
  const bool ok = allocLocked(phase_count, out_ch);
  unlock();
  return ok;
}

void motorStoreAuthGet(char *user, char *pass) {
  lock();
  strncpy(user, s_user, AUTH_USER_LEN);
  strncpy(pass, s_pass, AUTH_PASS_LEN);
  user[AUTH_USER_LEN - 1] = 0;
  pass[AUTH_PASS_LEN - 1] = 0;
  unlock();
}

bool motorStoreAuthSet(const char *user, const char *pass) {
  if (!user || !pass || !user[0] || !pass[0]) {
    return false;
  }
  lock();
  strncpy(s_user, user, AUTH_USER_LEN - 1);
  strncpy(s_pass, pass, AUTH_PASS_LEN - 1);
  s_user[AUTH_USER_LEN - 1] = 0;
  s_pass[AUTH_PASS_LEN - 1] = 0;
  s_prefs.putString(NVS_KEY_USER, s_user);
  s_prefs.putString(NVS_KEY_PASS, s_pass);
  unlock();
  return true;
}

int motorStoreAdd(const MotorRecord *in, char *err, size_t err_len) {
  MotorRecord rec = *in;
  rec.name[NAME_LEN - 1] = 0;
  if (!validateRecord(&rec, err, err_len)) {
    return -1;
  }
  lock();
  int slot = -1;
  for (int i = 0; i < MAX_MOTORS; i++) {
    if (!s_blob.motors[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    unlock();
    snprintf(err, err_len, "motor list full");
    return -1;
  }
  uint8_t ch[MAX_PHASES];
  if (!allocLocked(rec.phase_count, ch)) {
    unlock();
    snprintf(err, err_len, "not enough free channels");
    return -1;
  }
  memcpy(rec.channels, ch, sizeof(ch));
  rec.used = 1;
  if (rec.is_ac) {
    rec.uv_volts = 0;
    rec.ov_volts = 0;
  } else {
    rec.rated_ac_v = 0;
    rec.mains_hz = 0;
  }
  s_blob.motors[slot] = rec;
  unlock();
  if (!motorStoreSave()) {
    snprintf(err, err_len, "NVS save failed");
    return -1;
  }
  return slot;
}

bool motorStoreEdit(int idx, const MotorRecord *in, char *err, size_t err_len) {
  if (idx < 0 || idx >= MAX_MOTORS) {
    snprintf(err, err_len, "bad index");
    return false;
  }
  MotorRecord rec = *in;
  rec.name[NAME_LEN - 1] = 0;
  if (!validateRecord(&rec, err, err_len)) {
    return false;
  }
  lock();
  if (!s_blob.motors[idx].used) {
    unlock();
    snprintf(err, err_len, "empty slot");
    return false;
  }
  MotorRecord old = s_blob.motors[idx];
  rec.used = 1;
  rec.phase_count = s_blob.motors[idx].phase_count;
  memcpy(rec.channels, s_blob.motors[idx].channels, sizeof(rec.channels));
  if (rec.is_ac) {
    rec.uv_volts = 0;
    rec.ov_volts = 0;
  } else {
    rec.rated_ac_v = 0;
    rec.mains_hz = 0;
  }
  s_blob.motors[idx] = rec;
  unlock();
  if (!motorStoreSave()) {
    lock();
    s_blob.motors[idx] = old;
    unlock();
    snprintf(err, err_len, "NVS save failed");
    return false;
  }
  return true;
}

bool motorStoreDelete(int idx, MotorStatus status, char *err, size_t err_len) {
  if (idx < 0 || idx >= MAX_MOTORS) {
    snprintf(err, err_len, "bad index");
    return false;
  }
  if (status == MST_RUNNING || status == MST_COOLING) {
    snprintf(err, err_len, "delete only from Stopped or Fault");
    return false;
  }
  lock();
  if (!s_blob.motors[idx].used) {
    unlock();
    snprintf(err, err_len, "empty slot");
    return false;
  }
  MotorRecord old = s_blob.motors[idx];
  memset(&s_blob.motors[idx], 0, sizeof(MotorRecord));
  for (int p = 0; p < MAX_PHASES; p++) {
    s_blob.motors[idx].channels[p] = CH_UNUSED;
    s_blob.motors[idx].relay_active_high[p] = 1;
  }
  unlock();
  if (!motorStoreSave()) {
    lock();
    s_blob.motors[idx] = old;
    unlock();
    snprintf(err, err_len, "NVS save failed");
    return false;
  }
  return true;
}
