#pragma once

#include <stdint.h>
#include "config_limits.h"

enum MotorStatus : uint8_t {
  MST_STOPPED = 0,
  MST_RUNNING,
  MST_FAULT,
  MST_COOLING
};

enum FaultType : uint8_t {
  FT_NONE = 0,
  FT_I2T,
  FT_STALL,
  FT_SENSOR,
  FT_UNDERVOLT,
  FT_OVERVOLT,
  FT_NO_CURRENT
};

enum JamPhase : uint8_t {
  JAM_IDLE = 0,
  JAM_OFF  = 1,
  JAM_WAIT = 2
};

enum CmdType : uint8_t {
  CMD_START = 0,
  CMD_STOP,
  CMD_RESET,
  CMD_CALIBRATE,
  CMD_RELOAD
};

enum ToneId : uint8_t {
  TONE_NONE = 0,
  TONE_POWERUP,
  TONE_FAULT,
  TONE_ADDED,
  TONE_STARTED,
  TONE_STOPPED,
  TONE_CLICK,
  TONE_BACK,
  TONE_SILENCE
};

struct MotorRecord {
  uint8_t used;
  char    name[NAME_LEN];
  uint8_t phase_count;
  uint8_t channels[MAX_PHASES];
  uint8_t is_ac;
  uint8_t mains_hz;
  float   in_amps;
  float   stall_amps;
  float   start_current;
  uint32_t icd_ms;
  float   cooling_s;
  uint8_t auto_restart;
  uint8_t relay_active_high[MAX_PHASES];
  uint8_t step_count;
  float   step_k[MAX_STEPS];
  float   step_t_s[MAX_STEPS];
  float   rated_ac_v;
  float   uv_volts;
  float   ov_volts;
  uint8_t stall_recovery;
  uint8_t voltage_channel;
};

struct MotorBlob {
  uint32_t magic;
  uint16_t schema_version;
  uint16_t reserved;
  MotorRecord motors[MAX_MOTORS];
};

struct Command {
  CmdType type;
  uint8_t motor_idx;
};

struct LogEvent {
  uint32_t uptime_ms;
  char     motor[NAME_LEN];
  FaultType type;
  float    current_a;
  float    voltage_v;
  float    power;
  uint8_t  power_is_w;
};

struct ChannelRuntime {
  float    zero_mv;
  float    last_rms;
  float    energy_a2s;
  uint32_t last_ms;
  uint8_t  calibrated;
  float    v_zero;
  float    last_v;
  uint8_t  v_calibrated;
  float    v_filt;
  uint8_t  v_filt_valid;
};

struct MotorRuntime {
  MotorStatus status;
  uint32_t    run_start_ms;
  uint32_t    cooling_deadline_ms;
  uint32_t    fault_count;
  FaultType   last_fault;
  float       thermal_pct;
  float       power;
  float       energy;
  uint8_t     power_is_w;
  uint8_t     restart_count;
  uint32_t    low_current_ms;
  uint8_t     sensor_fault_count;
  uint32_t    sensor_fault_first_ms;
  uint8_t     jam_count;
  uint8_t     jam_phase;
  uint32_t    jam_deadline_ms;
};

struct StatusSnapshot {
  uint8_t       sd_ok;
  uint8_t       ads_ok[2];
  uint32_t      free_heap;
  uint8_t       calibrated;
  MotorRecord   motors[MAX_MOTORS];
  MotorRuntime  rt[MAX_MOTORS];
  float         rms[MAX_CHANNELS];
  float         volts[MAX_CHANNELS];
  uint8_t       v_calibrated[MAX_CHANNELS];
  float         thermal_pct[MAX_MOTORS];
};
