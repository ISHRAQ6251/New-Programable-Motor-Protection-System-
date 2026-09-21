#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <esp_task_wdt.h>
#include "protection.h"
#include "config_limits.h"
#include "sensing.h"
#include "voltage.h"
#include "relays.h"
#include "motor_store.h"
#include "config_pins.h"

struct SampleJob {
  uint8_t used;
  uint8_t mi;
  uint8_t phase_count;
  uint8_t is_ac;
  uint8_t mains_hz;
  uint8_t voltage_channel;
  uint8_t channels[MAX_PHASES];
  SampleResult res[MAX_PHASES];
  VoltageSample vres[MAX_PHASES];
  VoltageSample vsense;
};

static uint32_t s_volt_log_ms[MAX_MOTORS];

static uint8_t voltageChOf(const MotorRecord *m) {
  if (!m) {
    return CH_UNUSED;
  }
  if (m->voltage_channel < MAX_CHANNELS) {
    return m->voltage_channel;
  }
  if (m->phase_count > 0 && m->channels[0] < MAX_CHANNELS) {
    return m->channels[0];
  }
  return CH_UNUSED;
}

static MotorRecord s_motors[MAX_MOTORS];
static MotorRuntime s_rt[MAX_MOTORS];
static ChannelRuntime s_ch[MAX_CHANNELS];
static QueueHandle_t s_cmd_q = nullptr;
static QueueHandle_t s_log_q = nullptr;
static SemaphoreHandle_t s_mu = nullptr;
static volatile ToneId s_tone = TONE_NONE;
static volatile uint8_t s_calibrated = 0;
static volatile uint8_t s_sd_ok = 0;

// Non-destructive copy of the most recent trips, for the local panel and the
// web log page when SD is missing. s_log_q is single-consumer (loop() drains
// it to SD); readers must never steal from it. Writes happen in pushLog(),
// which its callers run with s_mu held. LOG_RING is 32 (config_limits.h).
static LogEvent s_log_ring[LOG_RING];
static int s_log_head = 0;
static int s_log_count = 0;

static void setTone(ToneId id) {
  s_tone = id;
}

static void pushLog(int mi, FaultType ft, float current_a, float voltage_v,
                    float power, uint8_t power_is_w) {
  LogEvent ev;
  ev.uptime_ms = millis();
  ev.type = ft;
  ev.current_a = current_a;
  ev.voltage_v = voltage_v;
  ev.power = power;
  ev.power_is_w = power_is_w;
  strncpy(ev.motor, s_motors[mi].name, NAME_LEN - 1);
  ev.motor[NAME_LEN - 1] = 0;
  if (s_log_q) {
    xQueueSend(s_log_q, &ev, 0);
  }
  s_log_ring[s_log_head] = ev;
  s_log_head = (s_log_head + 1) % LOG_RING;
  if (s_log_count < LOG_RING) {
    s_log_count++;
  }
}

static int lowestKIndex(const MotorRecord *m) {
  int best = 0;
  for (int i = 1; i < m->step_count; i++) {
    if (m->step_k[i] < m->step_k[best]) {
      best = i;
    }
  }
  return best;
}

static int activeStep(const MotorRecord *m, float irms) {
  int best = -1;
  float best_k = -1.0f;
  for (int i = 0; i < m->step_count; i++) {
    if (irms >= m->step_k[i] * m->in_amps && m->step_k[i] >= best_k) {
      best_k = m->step_k[i];
      best = i;
    }
  }
  return best;
}

static float eTrip(const MotorRecord *m, int step) {
  const float i = m->step_k[step] * m->in_amps;
  return i * i * m->step_t_s[step];
}

static void motorRelaysOff(int mi) {
  relaysMotorOff(s_motors[mi].channels, s_motors[mi].relay_active_high, s_motors[mi].phase_count);
}

static void motorRelaysOn(int mi) {
  relaysMotorOn(s_motors[mi].channels, s_motors[mi].relay_active_high, s_motors[mi].phase_count);
}

static void zeroEnergy(int mi) {
  for (int p = 0; p < s_motors[mi].phase_count; p++) {
    const uint8_t c = s_motors[mi].channels[p];
    if (c < MAX_CHANNELS) {
      s_ch[c].energy_a2s = 0;
      s_ch[c].last_rms = 0;
      s_ch[c].last_v = 0;
      s_ch[c].v_filt = 0;
      s_ch[c].v_filt_valid = 0;
    }
  }
  {
    const uint8_t vc = voltageChOf(&s_motors[mi]);
    if (vc < MAX_CHANNELS) {
      s_ch[vc].last_v = 0;
      s_ch[vc].v_filt = 0;
      s_ch[vc].v_filt_valid = 0;
    }
  }
  s_rt[mi].thermal_pct = 0;
  s_rt[mi].power = 0;
  s_rt[mi].energy = 0;
  s_rt[mi].low_current_ms = 0;
  s_rt[mi].jam_count = 0;
  s_rt[mi].jam_phase = JAM_IDLE;
  s_rt[mi].jam_deadline_ms = 0;
}

static void trip(int mi, FaultType ft, float current_a, float voltage_v) {
  const float window_power = s_rt[mi].power;
  const uint8_t window_is_w = s_rt[mi].power_is_w;
  motorRelaysOff(mi);
  s_rt[mi].status = MST_COOLING;
  s_rt[mi].last_fault = ft;
  s_rt[mi].fault_count++;
  s_rt[mi].restart_count++;
  float cool_s = s_motors[mi].cooling_s;
  if (cool_s > COOLING_MAX_S) {
    cool_s = COOLING_MAX_S;
  }
  s_rt[mi].cooling_deadline_ms = millis() + (uint32_t)(cool_s * 1000.0f);
  zeroEnergy(mi);
  pushLog(mi, ft, current_a, voltage_v, window_power, window_is_w);
  setTone(TONE_FAULT);
  const char *ftn = "UNKNOWN";
  switch (ft) {
    case FT_I2T:        ftn = "I2T"; break;
    case FT_STALL:      ftn = "STALL"; break;
    case FT_SENSOR:     ftn = "SENSOR_FAULT"; break;
    case FT_UNDERVOLT:  ftn = "UNDERVOLT"; break;
    case FT_OVERVOLT:   ftn = "OVERVOLT"; break;
    case FT_NO_CURRENT: ftn = "NO_CURRENT"; break;
    default: break;
  }
  Serial.printf("TRIP: motor=%s type=%s I=%.3f A V=%.3f\n",
                s_motors[mi].name, ftn, (double)current_a, (double)voltage_v);
}

static bool channelAdsReady(uint8_t c) {
  if (c >= MAX_CHANNELS) {
    return false;
  }
  return voltageAdsOk(c < 4 ? 0 : 1) && s_ch[c].v_calibrated;
}

static bool motorAdsReady(const MotorRecord *m) {
  if (!m || m->is_ac) {
    return true;
  }
  if (m->voltage_channel < MAX_CHANNELS) {
    return channelAdsReady(m->voltage_channel);
  }
  for (int p = 0; p < m->phase_count; p++) {
    const uint8_t c = m->channels[p];
    if (c >= MAX_CHANNELS) {
      continue;
    }
    if (!channelAdsReady(c)) {
      return false;
    }
  }
  return true;
}

static bool allIdleLocked() {
  for (int i = 0; i < MAX_MOTORS; i++) {
    if (!s_motors[i].used) {
      continue;
    }
    if (s_rt[i].status == MST_RUNNING || s_rt[i].status == MST_COOLING) {
      return false;
    }
  }
  return true;
}

static void handleCmd(const Command &cmd) {
  if (cmd.type == CMD_CALIBRATE) {
    return;
  }
  if (cmd.type == CMD_RELOAD) {
    MotorRecord tmp[MAX_MOTORS];
    motorStoreGet(tmp);
    for (int i = 0; i < MAX_MOTORS; i++) {
      const bool was_used = s_motors[i].used;
      const bool now_used = tmp[i].used;
      if (was_used && !now_used) {
        motorRelaysOff(i);
        s_rt[i].status = MST_STOPPED;
        zeroEnergy(i);
      }
      s_motors[i] = tmp[i];
      if (!now_used) {
        memset(&s_rt[i], 0, sizeof(s_rt[i]));
      } else if (s_rt[i].status != MST_RUNNING) {
        motorRelaysOff(i);
      }
    }
    return;
  }
  if (cmd.motor_idx >= MAX_MOTORS) {
    return;
  }
  const int mi = cmd.motor_idx;
  if (!s_motors[mi].used) {
    return;
  }
  if (cmd.type == CMD_START) {
    if (s_rt[mi].status != MST_STOPPED) {
      return;
    }
    if (!s_calibrated) {
      return;
    }
    if (!motorAdsReady(&s_motors[mi])) {
      return;
    }
    zeroEnergy(mi);
    s_rt[mi].status = MST_RUNNING;
    s_rt[mi].run_start_ms = millis();
    s_rt[mi].last_fault = FT_NONE;
    s_rt[mi].restart_count = 0;
    s_rt[mi].jam_count = 0;
    s_rt[mi].jam_phase = JAM_IDLE;
    s_rt[mi].jam_deadline_ms = 0;
    motorRelaysOn(mi);
    setTone(TONE_STARTED);
  } else if (cmd.type == CMD_STOP) {
    if (s_rt[mi].status != MST_RUNNING) {
      return;
    }
    motorRelaysOff(mi);
    s_rt[mi].status = MST_STOPPED;
    zeroEnergy(mi);
    setTone(TONE_STOPPED);
  } else if (cmd.type == CMD_RESET) {
    if (s_rt[mi].status != MST_FAULT && s_rt[mi].status != MST_COOLING) {
      return;
    }
    motorRelaysOff(mi);
    s_rt[mi].status = MST_STOPPED;
    zeroEnergy(mi);
    s_rt[mi].restart_count = 0;
    setTone(TONE_SILENCE);
  }
}

static void applySample(int mi, int p, const SampleResult &s, const VoltageSample *vs,
                        uint32_t now, float dt, float *hottest, FaultType *trip_ft,
                        float *trip_i, float *trip_v) {
  MotorRecord *m = &s_motors[mi];
  const uint8_t c = m->channels[p];
  if (c >= MAX_CHANNELS) {
    return;
  }
  s_ch[c].last_rms = s.rms;
  s_ch[c].last_ms = now;
  if (vs && vs->present) {
    if (!vs->fault) {
      if (s_ch[c].v_filt_valid) {
        s_ch[c].v_filt = 0.9f * s_ch[c].v_filt + 0.1f * vs->v_bus;
      } else {
        s_ch[c].v_filt = vs->v_bus;
        s_ch[c].v_filt_valid = 1;
      }
      s_ch[c].last_v = s_ch[c].v_filt;
    } else {
      s_ch[c].last_v = vs->v_bus;
    }
  }

  if (*trip_ft != FT_NONE) {
    return;
  }
  if (s.out_of_range || fabsf(s.rms) > SENSOR_I_CAP || s.stuck) {
    *trip_ft = FT_SENSOR;
    *trip_i = s.rms;
    if (vs) {
      *trip_v = vs->v_bus;
    }
    return;
  }
  MotorRuntime *rt = &s_rt[mi];
  if (s.rms >= m->stall_amps) {
    if (rt->jam_phase == JAM_IDLE) {
      const bool can_jam = m->stall_recovery &&
                           rt->jam_count < JAM_RELEASE_MAX &&
                           (uint32_t)(now - rt->run_start_ms) >= JAM_RELEASE_MIN_RUN_MS;
      if (can_jam) {
        motorRelaysOff(mi);
        rt->jam_phase = JAM_OFF;
        rt->jam_deadline_ms = now + JAM_RELEASE_OFF_MS;
      } else {
        *trip_ft = FT_STALL;
        *trip_i = s.rms;
        if (vs) {
          *trip_v = vs->v_bus;
        }
        return;
      }
    }
  }
  if (vs && !m->is_ac) {
    if (!vs->present || vs->fault) {
      *trip_ft = FT_SENSOR;
      *trip_i = s.rms;
      *trip_v = vs->v_bus;
      return;
    }
    if (rt->jam_phase == JAM_IDLE) {
      const uint32_t age = now - rt->run_start_ms;
      if (age >= UV_GRACE_MS) {
        const float v = s_ch[c].v_filt_valid ? s_ch[c].v_filt : vs->v_bus;
        if (m->ov_volts > 0.0f && v > m->ov_volts) {
          *trip_ft = FT_OVERVOLT;
          *trip_i = s.rms;
          *trip_v = v;
          return;
        }
        if (m->uv_volts > 0.0f && v < m->uv_volts) {
          *trip_ft = FT_UNDERVOLT;
          *trip_i = s.rms;
          *trip_v = v;
          return;
        }
      }
    }
  }

  const int lo = lowestKIndex(m);
  const float pickup = m->step_k[lo] * m->in_amps;
  if (s.rms >= pickup) {
    s_ch[c].energy_a2s += s.rms * s.rms * dt;
    const int st = activeStep(m, s.rms);
    if (st >= 0) {
      const float et = eTrip(m, st);
      if (et > 0.0f) {
        const float pct = 100.0f * s_ch[c].energy_a2s / et;
        if (pct > *hottest) {
          *hottest = pct;
        }
        if (s_ch[c].energy_a2s >= et) {
          *trip_ft = FT_I2T;
          *trip_i = s.rms;
          if (vs) {
            *trip_v = vs->v_bus;
          }
        }
      }
    }
  } else {
    if (m->cooling_s > 0.0f) {
      s_ch[c].energy_a2s *= fmaxf(0.0f, 1.0f - dt / m->cooling_s);
    } else {
      s_ch[c].energy_a2s = 0;
    }
    if (s_ch[c].energy_a2s < 0) {
      s_ch[c].energy_a2s = 0;
    }
  }
}

static void applyVoltageSense(int mi, const VoltageSample *vs, uint32_t now,
                              FaultType *trip_ft, float *trip_i, float *trip_v) {
  MotorRecord *m = &s_motors[mi];
  if (!vs || m->is_ac || *trip_ft != FT_NONE) {
    return;
  }
  const uint8_t c = voltageChOf(m);
  if (c >= MAX_CHANNELS) {
    return;
  }
  if (vs->present) {
    if (!vs->fault) {
      if (s_ch[c].v_filt_valid) {
        s_ch[c].v_filt = 0.9f * s_ch[c].v_filt + 0.1f * vs->v_bus;
      } else {
        s_ch[c].v_filt = vs->v_bus;
        s_ch[c].v_filt_valid = 1;
      }
      s_ch[c].last_v = s_ch[c].v_filt;
    } else {
      s_ch[c].last_v = vs->v_bus;
    }
  }
  if (!vs->present || vs->fault) {
    *trip_ft = FT_SENSOR;
    *trip_v = vs->v_bus;
    return;
  }
  MotorRuntime *rt = &s_rt[mi];
  if (rt->jam_phase != JAM_IDLE) {
    return;
  }
  const uint32_t age = now - rt->run_start_ms;
  if (age < UV_GRACE_MS) {
    return;
  }
  const float v = s_ch[c].v_filt_valid ? s_ch[c].v_filt : vs->v_bus;
  if (m->ov_volts > 0.0f && v > m->ov_volts) {
    *trip_ft = FT_OVERVOLT;
    *trip_v = v;
    return;
  }
  if (m->uv_volts > 0.0f && v < m->uv_volts) {
    *trip_ft = FT_UNDERVOLT;
    *trip_v = v;
  }
}

static void computePower(int mi, const SampleJob &job, float dt) {
  const MotorRecord *m = &s_motors[mi];
  MotorRuntime *rt = &s_rt[mi];
  if (m->is_ac) {
    float sum_i = 0;
    for (int p = 0; p < job.phase_count; p++) {
      sum_i += job.res[p].rms;
    }
    if (sum_i < 0.0f) {
      sum_i = 0.0f;
    }
    float s_va;
    if (m->phase_count >= 3) {
      s_va = (m->rated_ac_v / 1.7320508f) * sum_i;
    } else {
      s_va = m->rated_ac_v * sum_i;
    }
    if (s_va < 0.0f) {
      s_va = 0.0f;
    }
    rt->power = s_va;
    rt->power_is_w = 0;
    rt->energy += s_va * dt / 3600.0f;
  } else {
    float p = 0.0f;
    const uint8_t vc = voltageChOf(m);
    if (m->voltage_channel < MAX_CHANNELS && vc < MAX_CHANNELS) {
      float sum_i = 0.0f;
      for (int ch = 0; ch < job.phase_count; ch++) {
        sum_i += job.res[ch].rms;
      }
      p = s_ch[vc].last_v * sum_i;
    } else {
      for (int ch = 0; ch < job.phase_count; ch++) {
        const uint8_t c = job.channels[ch];
        const float v = (c < MAX_CHANNELS) ? s_ch[c].last_v : 0.0f;
        p += v * job.res[ch].rms;
      }
    }
    if (p < 0.0f) {
      p = 0.0f;
    }
    rt->power = p;
    rt->power_is_w = 1;
    rt->energy += p * dt / 3600.0f;
  }
}

static void applyMotorSample(SampleJob *job, uint32_t now, float dt) {
  const int mi = job->mi;
  if (s_rt[mi].status != MST_RUNNING) {
    return;
  }
  float hottest = 0;
  FaultType trip_ft = FT_NONE;
  float trip_i = 0;
  float trip_v = 0;
  const bool dedicated_v = !job->is_ac && job->voltage_channel < MAX_CHANNELS;
  for (int p = 0; p < job->phase_count; p++) {
    const VoltageSample *vs = (job->is_ac || dedicated_v) ? nullptr : &job->vres[p];
    applySample(mi, p, job->res[p], vs, now, dt, &hottest, &trip_ft, &trip_i, &trip_v);
  }
  if (dedicated_v) {
    applyVoltageSense(mi, &job->vsense, now, &trip_ft, &trip_i, &trip_v);
  }
  computePower(mi, *job, dt);
  s_rt[mi].thermal_pct = hottest;

  if ((uint32_t)(now - s_rt[mi].run_start_ms) >= CLEAN_RUN_MS) {
    s_rt[mi].restart_count = 0;
  }

  float max_i = 0;
  for (int p = 0; p < job->phase_count; p++) {
    if (job->res[p].rms > max_i) {
      max_i = job->res[p].rms;
    }
  }
  if (s_rt[mi].jam_phase != JAM_IDLE) {
    s_rt[mi].low_current_ms = 0;
  } else if (max_i < NO_CURRENT_FRAC * s_motors[mi].in_amps) {
    if (s_rt[mi].low_current_ms == 0) {
      s_rt[mi].low_current_ms = now;
    }
    if (trip_ft == FT_NONE &&
        (uint32_t)(now - s_rt[mi].low_current_ms) >= NO_CURRENT_MS) {
      trip_ft = FT_NO_CURRENT;
      trip_i = max_i;
      if (!job->is_ac) {
        const uint8_t vc = voltageChOf(&s_motors[mi]);
        if (vc < MAX_CHANNELS) {
          trip_v = s_ch[vc].last_v;
        }
      }
    }
  } else {
    s_rt[mi].low_current_ms = 0;
  }

  if (trip_ft != FT_NONE) {
    trip(mi, trip_ft, trip_i, trip_v);
  } else if (!job->is_ac &&
             (uint32_t)(now - s_volt_log_ms[mi]) >= VOLT_LOG_MS) {
    s_volt_log_ms[mi] = now;
    const uint8_t vc = voltageChOf(&s_motors[mi]);
    float raw = 0;
    float adc = 0;
    if (dedicated_v) {
      raw = job->vsense.v_bus;
      adc = job->vsense.v_adc;
    } else if (job->vres[0].present) {
      raw = job->vres[0].v_bus;
      adc = job->vres[0].v_adc;
    }
    const float zero = (vc < MAX_CHANNELS) ? s_ch[vc].v_zero : 0;
    const float filt = (vc < MAX_CHANNELS) ? s_ch[vc].last_v : 0;
    Serial.printf("VOLT: ch=%u adc=%.3f zero=%.3f bus=%.3f filt=%.2f uv=%.1f ov=%.1f\n",
                  (unsigned)vc, (double)adc, (double)zero, (double)raw, (double)filt,
                  (double)s_motors[mi].uv_volts, (double)s_motors[mi].ov_volts);
  }
}

static void processJamRelease(int mi, uint32_t now) {
  MotorRecord *m = &s_motors[mi];
  MotorRuntime *rt = &s_rt[mi];
  if (rt->status != MST_RUNNING || rt->jam_phase == JAM_IDLE) {
    return;
  }
  if ((int32_t)(now - rt->jam_deadline_ms) < 0) {
    return;
  }
  if (rt->jam_phase == JAM_OFF) {
    motorRelaysOn(mi);
    rt->jam_phase = JAM_WAIT;
    rt->jam_deadline_ms = now + JAM_RELEASE_WAIT_MS;
    if (rt->jam_count < JAM_RELEASE_MAX) {
      rt->jam_count++;
    }
    return;
  }
  if (rt->jam_phase != JAM_WAIT) {
    return;
  }
  bool still = false;
  float trip_i = 0;
  float trip_v = 0;
  for (int p = 0; p < m->phase_count; p++) {
    const uint8_t c = m->channels[p];
    if (c >= MAX_CHANNELS) {
      continue;
    }
    if (s_ch[c].last_rms >= m->stall_amps) {
      still = true;
      if (s_ch[c].last_rms > trip_i) {
        trip_i = s_ch[c].last_rms;
        const uint8_t vc = voltageChOf(m);
        trip_v = (vc < MAX_CHANNELS) ? s_ch[vc].last_v : s_ch[c].last_v;
      }
    }
  }
  if (!still) {
    rt->jam_count = 0;
    rt->jam_phase = JAM_IDLE;
    rt->jam_deadline_ms = 0;
    return;
  }
  if (rt->jam_count < JAM_RELEASE_MAX) {
    motorRelaysOff(mi);
    rt->jam_phase = JAM_OFF;
    rt->jam_deadline_ms = now + JAM_RELEASE_OFF_MS;
    return;
  }
  trip(mi, FT_STALL, trip_i, trip_v);
}

static void processMotorCooling(int mi, uint32_t now) {
  MotorRecord *m = &s_motors[mi];
  MotorRuntime *rt = &s_rt[mi];
  if ((int32_t)(now - rt->cooling_deadline_ms) >= 0) {
    if (m->auto_restart && rt->restart_count < MAX_RESTARTS) {
      if (!motorAdsReady(m)) {
        rt->status = MST_FAULT;
        setTone(TONE_FAULT);
      } else {
        zeroEnergy(mi);
        rt->status = MST_RUNNING;
        rt->run_start_ms = now;
        rt->last_fault = FT_NONE;
        motorRelaysOn(mi);
        setTone(TONE_STARTED);
      }
    } else {
      rt->status = MST_FAULT;
      setTone(TONE_FAULT);
    }
  }
}

static void protectionWdtBegin() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
    esp_task_wdt_init(&cfg);
  }
}

void protectionMutexInit() {
  if (!s_mu) {
    s_mu = xSemaphoreCreateMutex();
  }
  if (!s_cmd_q) {
    s_cmd_q = xQueueCreate(16, sizeof(Command));
  }
  if (!s_log_q) {
    s_log_q = xQueueCreate(16, sizeof(LogEvent));
  }
}

void protectionBegin() {
  memset(s_motors, 0, sizeof(s_motors));
  memset(s_rt, 0, sizeof(s_rt));
  memset(s_ch, 0, sizeof(s_ch));
  motorStoreGet(s_motors);
  sensingCalibrateAll(s_ch);
  voltageCalibrateAll(s_ch);
  s_calibrated = 1;
  protectionWdtBegin();
}

void protectionTask(void *arg) {
  (void)arg;
  uint32_t last = millis();
  uint32_t heap_last = 0;
  esp_task_wdt_add(nullptr);
  for (;;) {
    Command cmd;
    bool want_cal = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
      if (cmd.type == CMD_CALIBRATE) {
        want_cal = allIdleLocked();
      } else {
        handleCmd(cmd);
      }
    }
    xSemaphoreGive(s_mu);

    if (want_cal) {
      ChannelRuntime tmpch[MAX_CHANNELS];
      bool idle = false;
      xSemaphoreTake(s_mu, portMAX_DELAY);
      idle = allIdleLocked();
      if (idle) {
        memcpy(tmpch, s_ch, sizeof(tmpch));
      }
      xSemaphoreGive(s_mu);
      if (idle) {
        Serial.println("CAL: re-probe ADS1115 then zeros");
        voltageReprobe();
        sensingCalibrateAll(tmpch);
        voltageCalibrateAll(tmpch);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        memcpy(s_ch, tmpch, sizeof(tmpch));
        s_calibrated = 1;
        xSemaphoreGive(s_mu);
      }
    }

    const uint32_t now = millis();
    float dt = (now - last) / 1000.0f;
    if (dt < 0.001f) {
      dt = 0.001f;
    }
    if (dt > DT_MAX_S) {
      dt = DT_MAX_S;
    }
    last = now;

    SampleJob jobs[MAX_MOTORS];
    memset(jobs, 0, sizeof(jobs));
    int nj = 0;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_MOTORS; i++) {
      if (!s_motors[i].used) {
        continue;
      }
      if (s_rt[i].status != MST_RUNNING) {
        continue;
      }
      jobs[nj].used = 1;
      jobs[nj].mi = (uint8_t)i;
      jobs[nj].phase_count = s_motors[i].phase_count;
      jobs[nj].is_ac = s_motors[i].is_ac;
      jobs[nj].mains_hz = s_motors[i].mains_hz;
      jobs[nj].voltage_channel = s_motors[i].voltage_channel;
      memcpy(jobs[nj].channels, s_motors[i].channels, sizeof(jobs[nj].channels));
      nj++;
    }
    ChannelRuntime chcopy[MAX_CHANNELS];
    memcpy(chcopy, s_ch, sizeof(chcopy));
    xSemaphoreGive(s_mu);

    for (int j = 0; j < nj; j++) {
      SampleJob *job = &jobs[j];
      for (int p = 0; p < job->phase_count; p++) {
        const uint8_t c = job->channels[p];
        if (c >= MAX_CHANNELS) {
          continue;
        }
        job->res[p] = sensingSample((int)c, &chcopy[c], job->is_ac, job->mains_hz);
        if (!job->is_ac && job->voltage_channel >= MAX_CHANNELS) {
          job->vres[p] = voltageSample((int)c, &chcopy[c]);
        }
      }
      if (!job->is_ac && job->voltage_channel < MAX_CHANNELS) {
        job->vsense = voltageSample((int)job->voltage_channel, &chcopy[job->voltage_channel]);
      }
      xSemaphoreTake(s_mu, portMAX_DELAY);
      applyMotorSample(job, now, dt);
      xSemaphoreGive(s_mu);
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_MOTORS; i++) {
      if (!s_motors[i].used) {
        continue;
      }
      if (s_rt[i].status == MST_COOLING) {
        processMotorCooling(i, now);
        continue;
      }
      if (s_rt[i].status == MST_RUNNING) {
        processJamRelease(i, now);
      }
      if (s_rt[i].status != MST_RUNNING) {
        s_rt[i].thermal_pct = 0;
        s_rt[i].power = 0;
        s_rt[i].energy = 0;
        s_rt[i].power_is_w = s_motors[i].is_ac ? 0 : 1;
        s_rt[i].low_current_ms = 0;
        continue;
      }
    }
    xSemaphoreGive(s_mu);

    if (now - heap_last >= 1000) {
      heap_last = now;
      Serial.printf("HEAP: protect=%u\n", (unsigned)ESP.getFreeHeap());
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

bool protectionPost(CmdType type, uint8_t motor_idx) {
  Command c;
  c.type = type;
  c.motor_idx = motor_idx;
  return xQueueSend(s_cmd_q, &c, 0) == pdTRUE;
}

void protectionSnapshot(StatusSnapshot *out) {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  memset(out, 0, sizeof(*out));
  out->sd_ok = s_sd_ok;
  out->ads_ok[0] = voltageAdsOk(0) ? 1 : 0;
  out->ads_ok[1] = voltageAdsOk(1) ? 1 : 0;
  out->free_heap = ESP.getFreeHeap();
  out->calibrated = s_calibrated;
  memcpy(out->motors, s_motors, sizeof(s_motors));
  memcpy(out->rt, s_rt, sizeof(s_rt));
  for (int i = 0; i < MAX_CHANNELS; i++) {
    out->rms[i] = s_ch[i].last_rms;
    out->volts[i] = s_ch[i].last_v;
    out->v_calibrated[i] = s_ch[i].v_calibrated;
  }
  for (int i = 0; i < MAX_MOTORS; i++) {
    out->thermal_pct[i] = s_rt[i].thermal_pct;
    if (out->rt[i].status == MST_RUNNING && out->rt[i].run_start_ms) {
      out->rt[i].run_start_ms = millis() - s_rt[i].run_start_ms;
    } else {
      out->rt[i].run_start_ms = 0;
    }
  }
  xSemaphoreGive(s_mu);
}

bool protectionPopLog(LogEvent *out) {
  return xQueueReceive(s_log_q, out, 0) == pdTRUE;
}

int protectionCopyLog(LogEvent *out, int max) {
  if (!out || max <= 0) {
    return 0;
  }
  xSemaphoreTake(s_mu, portMAX_DELAY);
  int n = (s_log_count < max) ? s_log_count : max;
  for (int i = 0; i < n; i++) {
    const int idx = (s_log_head - 1 - i + 2 * LOG_RING) % LOG_RING;
    out[i] = s_log_ring[idx];
  }
  xSemaphoreGive(s_mu);
  return n;
}

void protectionClearLog() {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  s_log_head = 0;
  s_log_count = 0;
  xSemaphoreGive(s_mu);
}

// Shared Start gate: the web API and the local panel must enforce the same
// DC-readiness rule. Returns true when the command may be queued; when false,
// *reason holds a short human-readable cause for the caller to surface.
bool protectionCanStart(const StatusSnapshot &snap, int idx, const char **reason) {
  if (reason) {
    *reason = nullptr;
  }
  if (idx < 0 || idx >= MAX_MOTORS) {
    if (reason) {
      *reason = "bad motor index";
    }
    return false;
  }
  if (!snap.motors[idx].used || snap.motors[idx].is_ac) {
    return true;
  }
  bool ads_ok = true;
  bool vcal = true;
  const MotorRecord *m = &snap.motors[idx];
  if (m->voltage_channel < MAX_CHANNELS) {
    const uint8_t c = m->voltage_channel;
    if (!snap.ads_ok[c < 4 ? 0 : 1]) {
      ads_ok = false;
    }
    if (!snap.v_calibrated[c]) {
      vcal = false;
    }
  } else {
    for (int p = 0; p < m->phase_count; p++) {
      const uint8_t c = m->channels[p];
      if (c >= MAX_CHANNELS) {
        continue;
      }
      if (!snap.ads_ok[c < 4 ? 0 : 1]) {
        ads_ok = false;
      }
      if (!snap.v_calibrated[c]) {
        vcal = false;
      }
    }
  }
  if (!ads_ok) {
    if (reason) {
      *reason = "DC start needs the ADS1115 for this motor's channels";
    }
    return false;
  }
  if (!vcal) {
    if (reason) {
      *reason = "calibrate DC voltage zeros first";
    }
    return false;
  }
  return true;
}

ToneId protectionTakeTone() {
  ToneId t = s_tone;
  s_tone = TONE_NONE;
  return t;
}

bool protectionAllIdleForCal() {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  const bool ok = allIdleLocked();
  xSemaphoreGive(s_mu);
  return ok;
}

MotorStatus protectionMotorStatus(int idx) {
  if (idx < 0 || idx >= MAX_MOTORS) {
    return MST_STOPPED;
  }
  xSemaphoreTake(s_mu, portMAX_DELAY);
  const MotorStatus s = s_rt[idx].status;
  xSemaphoreGive(s_mu);
  return s;
}

void protectionSetSdOk(uint8_t ok) {
  s_sd_ok = ok;
}

void protectionCopyVcal(uint8_t out[MAX_CHANNELS]) {
  if (!out) {
    return;
  }
  xSemaphoreTake(s_mu, portMAX_DELAY);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    out[i] = s_ch[i].v_calibrated;
  }
  xSemaphoreGive(s_mu);
}
