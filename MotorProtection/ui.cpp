#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "ui_icons.h"
#include "config_pins.h"
#include "config_limits.h"
#include "types.h"
#include "protection.h"
#include "buzzer.h"
#include "voltage.h"
#include "motor_store.h"

// SH1106 on the primary I2C bus (Wire), shared with the ADS1115s. Hardware-I2C
// U8g2 constructors take only (rotation, reset) — pins are bound with
// Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL) before u8g2.begin() and re-bound after
// (Adafruit BusIO / U8g2 may call Wire.begin() with no pins). Never Wire.end().
// Every raw Wire transaction is held under i2cLock()/i2cUnlock() only for the
// duration of that call.
static U8G2_SH1106_128X64_NONAME_F_HW_I2C s_oled(U8G2_R0, U8X8_PIN_NONE);
static Adafruit_NeoPixel s_led(1, LED_PIN, NEO_GRB + NEO_KHZ800);

#define UI_REFRESH_MS     160
#define UI_ICON_MS        450
#define UI_SW_DEBOUNCE_MS 40
#define UI_LONG_MS        600
#define UI_MSG_MS         2200
#define UI_VIS_HOME       4
#define UI_VIS_FAULT      2

#define F_TITLE u8g2_font_helvB08_tr
#define F_BODY  u8g2_font_6x12_tr
#define F_SMALL u8g2_font_5x7_tr

static volatile uint8_t s_boot_stage = UI_BOOT_SPLASH;
static char s_boot_note[48] = "";
static uint32_t s_ui_start_ms = 0;
static uint32_t s_boot_done_ms = 0;

enum Screen : uint8_t {
  SC_SPLASH = 0, SC_HOME, SC_MOTOR, SC_FAULTLOG, SC_DIAG, SC_FW, SC_NET
};
static Screen s_screen = SC_SPLASH;

static uint8_t s_motor_order[MAX_MOTORS];
static int s_motor_n = 0;
static int s_home_sel = 0;
static int s_home_first = 0;
static int s_motor_sel = 0;
static int s_fault_sel = 0;
static int s_fault_first = 0;

static char s_msg[44] = "";
static uint32_t s_msg_until = 0;

static char s_net_user[AUTH_USER_LEN] = "";
static char s_net_pass[AUTH_PASS_LEN] = "";
static bool s_net_loaded = false;
static uint8_t s_icon_frame = 0;

// --- quadrature decoder -----------------------------------------------------
// State-table decoder: a single physical detent accumulates to +/-4 before an
// event fires, so contact bounce on the KY-040 cannot emit spurious steps.
static const int8_t kQTable[16] = {
  0, -1, 1, 0,
  1, 0, 0, -1,
  -1, 0, 0, 1,
  0, 1, -1, 0
};
static uint8_t s_enc_prev = 0;
static int8_t s_enc_acc = 0;

static uint8_t s_sw_stable = 1;
static uint8_t s_sw_last = 1;
static uint32_t s_sw_change_ms = 0;
static bool s_pressed = false;
static bool s_long_fired = false;
static uint32_t s_press_ms = 0;

enum { EV_NONE = 0, EV_CW, EV_CCW, EV_SHORT, EV_LONG };

// --- small helpers ----------------------------------------------------------

static const uint8_t *statusIcon(MotorStatus st) {
  const uint8_t f = (uint8_t)(s_icon_frame % 3);
  switch (st) {
    case MST_RUNNING: return ICON_RUNNING_F[f];
    case MST_FAULT:   return ICON_FAULT_F[f];
    case MST_COOLING: return ICON_COOLING_F[f];
    default:          return ICON_STOPPED;
  }
}

static const char *statusWord(MotorStatus st) {
  switch (st) {
    case MST_RUNNING: return "RUNNING";
    case MST_FAULT:   return "FAULT";
    case MST_COOLING: return "COOLING";
    default:          return "STOPPED";
  }
}

static const char *faultAbbrev(FaultType ft) {
  switch (ft) {
    case FT_I2T:       return "I2T";
    case FT_STALL:     return "STALL";
    case FT_SENSOR:    return "SENSOR";
    case FT_UNDERVOLT: return "UNDER-V";
    case FT_OVERVOLT:  return "OVER-V";
    case FT_NO_CURRENT:return "NO-I";
    default:           return "";
  }
}

static void setMsg(const char *m) {
  if (!m) {
    return;
  }
  strncpy(s_msg, m, sizeof(s_msg) - 1);
  s_msg[sizeof(s_msg) - 1] = 0;
  s_msg_until = millis() + UI_MSG_MS;
}

static int totalFaults(const StatusSnapshot &snap) {
  int total = 0;
  for (int i = 0; i < MAX_MOTORS; i++) {
    if (snap.motors[i].used) {
      total += (int)snap.rt[i].fault_count;
    }
  }
  return total > 999 ? 999 : total;
}

static void rebuildMotorOrder() {
  StatusSnapshot snap;
  protectionSnapshot(&snap);
  s_motor_n = 0;
  for (int i = 0; i < MAX_MOTORS; i++) {
    if (snap.motors[i].used && s_motor_n < MAX_MOTORS) {
      s_motor_order[s_motor_n++] = (uint8_t)i;
    }
  }
  if (s_home_sel >= s_motor_n + 4) {
    s_home_sel = s_motor_n + 3;
  }
  if (s_home_sel < 0) {
    s_home_sel = 0;
  }
  if (s_motor_sel >= s_motor_n) {
    s_motor_sel = (s_motor_n > 0) ? s_motor_n - 1 : 0;
  }
}

static void loadNet() {
  if (!s_net_loaded) {
    motorStoreAuthGet(s_net_user, s_net_pass);
    s_net_loaded = true;
  }
}

static int faultRank(FaultType ft) {
  switch (ft) {
    case FT_STALL:      return 6;
    case FT_SENSOR:     return 5;
    case FT_NO_CURRENT: return 4;
    case FT_OVERVOLT:   return 3;
    case FT_UNDERVOLT:  return 2;
    case FT_I2T:        return 1;
    default:            return 0;
  }
}

static uint32_t lerpRgb(uint32_t a, uint32_t b, float t) {
  if (t < 0) {
    t = 0;
  }
  if (t > 1) {
    t = 1;
  }
  const int ar = (int)((a >> 16) & 0xFF);
  const int ag = (int)((a >> 8) & 0xFF);
  const int ab = (int)(a & 0xFF);
  const int br = (int)((b >> 16) & 0xFF);
  const int bg = (int)((b >> 8) & 0xFF);
  const int bb = (int)(b & 0xFF);
  const uint8_t r = (uint8_t)(ar + (int)((br - ar) * t));
  const uint8_t g = (uint8_t)(ag + (int)((bg - ag) * t));
  const uint8_t bl = (uint8_t)(ab + (int)((bb - ab) * t));
  return s_led.Color(r, g, bl);
}

static uint32_t thermalColor(float pct) {
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  static const uint32_t stops[5] = {
    0x0000FF, 0x00FFFF, 0x00FF00, 0xFFFF00, 0xFF8000
  };
  const int seg = (pct >= 100.0f) ? 3 : (int)(pct / 25.0f);
  const float t = (pct - (float)seg * 25.0f) / 25.0f;
  return lerpRgb(stops[seg], stops[seg + 1], t);
}

static uint32_t faultColor(FaultType ft) {
  switch (ft) {
    case FT_STALL:      return s_led.Color(255, 0, 0);
    case FT_I2T:        return s_led.Color(255, 140, 0);
    case FT_SENSOR:     return s_led.Color(255, 0, 255);
    case FT_UNDERVOLT:  return s_led.Color(255, 255, 0);
    case FT_OVERVOLT:   return s_led.Color(255, 255, 255);
    case FT_NO_CURRENT: return s_led.Color(0, 255, 255);
    default:            return s_led.Color(255, 0, 0);
  }
}

static void updateStatusLed(const StatusSnapshot &snap) {
  MotorStatus worst = MST_STOPPED;
  FaultType worst_ft = FT_NONE;
  float hottest = 0;
  bool any = false;
  for (int i = 0; i < MAX_MOTORS; i++) {
    if (!snap.motors[i].used) {
      continue;
    }
    any = true;
    const MotorStatus st = snap.rt[i].status;
    if (st == MST_FAULT && worst != MST_FAULT) {
      worst = MST_FAULT;
      worst_ft = snap.rt[i].last_fault;
    } else if (st == MST_FAULT) {
      if (faultRank(snap.rt[i].last_fault) > faultRank(worst_ft)) {
        worst_ft = snap.rt[i].last_fault;
      }
    } else if (st == MST_RUNNING && worst != MST_FAULT) {
      worst = MST_RUNNING;
      if (snap.thermal_pct[i] > hottest) {
        hottest = snap.thermal_pct[i];
      }
    } else if (st == MST_COOLING && worst != MST_FAULT && worst != MST_RUNNING) {
      worst = MST_COOLING;
    }
  }
  if (!any || worst == MST_STOPPED) {
    s_led.setPixelColor(0, 0);
    s_led.show();
    return;
  }
  const bool flash_on = ((millis() / 250) & 1u) != 0;
  if (worst == MST_FAULT) {
    s_led.setPixelColor(0, flash_on ? faultColor(worst_ft) : 0);
  } else if (worst == MST_COOLING) {
    s_led.setPixelColor(0, flash_on ? s_led.Color(0, 0, 255) : 0);
  } else {
    s_led.setPixelColor(0, thermalColor(hottest));
  }
  s_led.show();
}

// --- drawing primitives -----------------------------------------------------

static void drawHeader(const char *title, int faults) {
  s_oled.setFont(F_TITLE);
  s_oled.drawStr(2, 10, title);
  if (faults > 0) {
    char b[8];
    s_oled.drawXBMP(96, 2, 8, 8, statusIcon(MST_FAULT));
    s_oled.setFont(F_SMALL);
    snprintf(b, sizeof(b), "%d", faults);
    s_oled.drawStr(106, 10, b);
  }
  s_oled.drawXBMP(118, 2, 8, 8, ICON_AP);
  s_oled.drawHLine(0, 12, 128);
}

static void drawScrollbar(int first, int visible, int total) {
  if (total <= visible) {
    return;
  }
  const int track_y = 14;
  const int track_h = 50;
  s_oled.drawVLine(127, track_y, track_h);
  int thumb_h = track_h * visible / total;
  if (thumb_h < 6) {
    thumb_h = 6;
  }
  const int max_first = total - visible;
  int thumb_y = track_y;
  if (max_first > 0) {
    thumb_y += (track_h - thumb_h) * first / max_first;
  }
  s_oled.drawBox(126, thumb_y, 2, thumb_h);
}

static void drawThermalBar(int x, int y, int w, int h, float pct) {
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  const int segs = 8;
  const int gap = 1;
  const int seg_w = (w - (segs - 1) * gap) / segs;
  const int filled = (int)(pct * segs / 100.0f + 0.5f);
  for (int i = 0; i < segs; i++) {
    const int sx = x + i * (seg_w + gap);
    if (i < filled) {
      s_oled.drawBox(sx, y, seg_w, h);
    } else {
      s_oled.drawFrame(sx, y, seg_w, h);
    }
  }
}

// --- screens ----------------------------------------------------------------

static int centerX(const char *s) {
  const int w = s_oled.getStrWidth(s);
  int x = (128 - w) / 2;
  return x < 0 ? 0 : x;
}

static void renderSplash() {
  const uint32_t elapsed = millis() - s_ui_start_ms;
  int reveal = (int)(elapsed / 6);
  if (reveal > 128) {
    reveal = 128;
  }
  s_oled.setFont(u8g2_font_helvB10_tr);
  s_oled.drawStr(centerX("MPS-505"), 22, "MPS-505");
  s_oled.setFont(F_SMALL);
  const char *note = s_boot_note[0] ? s_boot_note : "Motor Protection";
  s_oled.drawStr(centerX(note), 34, note);
  s_oled.setDrawColor(0);
  s_oled.drawBox(0, 10, 128 - reveal, 30);
  s_oled.setDrawColor(1);

  static const char *labels[4] = {"Relay fail-safe", "ADS1115 probe", "SD card", "Calibration"};
  for (int i = 0; i < 4; i++) {
    const int y = 43 + i * 5;
    s_oled.drawStr(2, y, labels[i]);
    if ((int)s_boot_stage >= i + 1) {
      s_oled.drawStr(110, y, "ok");
    }
  }
}

static void renderHome(const StatusSnapshot &snap) {
  drawHeader("Motors", totalFaults(snap));
  static const char *menus[4] = {"Fault Log", "Diagnostics", "Firmware Info", "Network Info"};
  const int rows = s_motor_n + 4;
  const int visible = UI_VIS_HOME;
  if (s_home_sel < s_home_first) {
    s_home_first = s_home_sel;
  }
  if (s_home_sel >= s_home_first + visible) {
    s_home_first = s_home_sel - visible + 1;
  }
  if (s_home_first > rows - visible) {
    s_home_first = rows - visible;
  }
  if (s_home_first < 0) {
    s_home_first = 0;
  }
  for (int j = 0; j < visible; j++) {
    const int row = s_home_first + j;
    if (row >= rows) {
      break;
    }
    const int y = 14 + j * 12;
    const bool sel = (row == s_home_sel);
    if (sel) {
      s_oled.drawBox(0, y, 124, 12);
      s_oled.setDrawColor(0);
    }
    if (row < s_motor_n) {
      const int mi = s_motor_order[row];
      s_oled.drawXBMP(2, y + 2, 8, 8, statusIcon(snap.rt[mi].status));
      s_oled.setFont(F_BODY);
      s_oled.drawStr(13, y + 10, snap.motors[mi].name);
    } else {
      s_oled.setFont(F_BODY);
      s_oled.drawStr(13, y + 10, menus[row - s_motor_n]);
    }
    if (sel) {
      s_oled.setDrawColor(1);
    }
  }
  drawScrollbar(s_home_first, visible, rows);
}

static void renderMotor(const StatusSnapshot &snap) {
  if (s_motor_n == 0) {
    drawHeader("Motors", 0);
    s_oled.setFont(F_BODY);
    s_oled.drawStr(centerX("No motors configured"), 34, "No motors configured");
    return;
  }
  if (s_motor_sel >= s_motor_n) {
    s_motor_sel = s_motor_n - 1;
  }
  const int mi = s_motor_order[s_motor_sel];
  const MotorRecord *m = &snap.motors[mi];
  const MotorRuntime *rt = &snap.rt[mi];
  drawHeader(m->name, totalFaults(snap));

  char b[40];
  int y = 23;
  s_oled.drawXBMP(2, y - 8, 8, 8, statusIcon(rt->status));
  s_oled.setFont(F_BODY);
  if (rt->status == MST_FAULT && rt->last_fault != FT_NONE) {
    snprintf(b, sizeof(b), "%s %s", statusWord(rt->status), faultAbbrev(rt->last_fault));
  } else {
    snprintf(b, sizeof(b), "%s", statusWord(rt->status));
  }
  s_oled.drawStr(13, y, b);

  float imax = 0;
  for (int p = 0; p < m->phase_count; p++) {
    const uint8_t c = m->channels[p];
    if (c < MAX_CHANNELS && snap.rms[c] > imax) {
      imax = snap.rms[c];
    }
  }
  y += 12;
  snprintf(b, sizeof(b), "I %.2f A", (double)imax);
  s_oled.drawStr(2, y, b);

  y += 12;
  drawThermalBar(2, y - 7, 92, 8, snap.thermal_pct[mi]);
  snprintf(b, sizeof(b), "%.0f%%", (double)snap.thermal_pct[mi]);
  s_oled.drawStr(98, y, b);

  y += 12;
  if (!m->is_ac) {
    const uint8_t c0 = m->channels[0];
    const float v = (c0 < MAX_CHANNELS) ? snap.volts[c0] : 0;
    snprintf(b, sizeof(b), "%.1f V  %.0f W", (double)v, (double)rt->power);
  } else {
    snprintf(b, sizeof(b), "%.0f V~  %.0f VA", (double)m->rated_ac_v, (double)rt->power);
  }
  s_oled.drawStr(2, y, b);
}

static void renderFaultLog() {
  LogEvent ev[16];
  const int n = protectionCopyLog(ev, 16);
  drawHeader("Fault Log", 0);
  if (n == 0) {
    s_oled.setFont(F_BODY);
    s_oled.drawStr(centerX("No trips logged"), 34, "No trips logged");
    return;
  }
  const int visible = UI_VIS_FAULT;
  if (s_fault_sel >= n) {
    s_fault_sel = n - 1;
  }
  if (s_fault_sel < 0) {
    s_fault_sel = 0;
  }
  if (s_fault_sel < s_fault_first) {
    s_fault_first = s_fault_sel;
  }
  if (s_fault_sel >= s_fault_first + visible) {
    s_fault_first = s_fault_sel - visible + 1;
  }
  if (s_fault_first > n - visible) {
    s_fault_first = n - visible;
  }
  if (s_fault_first < 0) {
    s_fault_first = 0;
  }
  for (int j = 0; j < visible; j++) {
    const int i = s_fault_first + j;
    if (i >= n) {
      break;
    }
    const int y = 14 + j * 24;
    const bool sel = (i == s_fault_sel);
    if (sel) {
      s_oled.drawBox(0, y, 124, 23);
      s_oled.setDrawColor(0);
    }
    s_oled.setFont(F_TITLE);
    s_oled.drawStr(2, y + 10, ev[i].motor);
    const char *fa = faultAbbrev(ev[i].type);
    s_oled.drawStr(122 - s_oled.getStrWidth(fa), y + 10, fa);
    s_oled.setFont(F_SMALL);
    char b[32];
    snprintf(b, sizeof(b), "%.2f A  %.1f V", (double)ev[i].current_a, (double)ev[i].voltage_v);
    s_oled.drawStr(2, y + 20, b);
    if (sel) {
      s_oled.setDrawColor(1);
    }
  }
  drawScrollbar(s_fault_first, visible, n);
}

static void renderDiag(const StatusSnapshot &snap) {
  drawHeader("Diagnostics", 0);
  s_oled.setFont(F_SMALL);
  char b[48];
  int y = 22;
  for (int chip = 0; chip < 2; chip++) {
    const uint8_t addr = chip ? ADS1115_ADDR_B : ADS1115_ADDR_A;
    if (snap.ads_ok[chip]) {
      snprintf(b, sizeof(b), "ADS 0x%02X ok", addr);
    } else {
      snprintf(b, sizeof(b), "ADS 0x%02X miss: %s", addr,
               voltageI2cErrLabel(voltageAdsErr(chip)));
    }
    s_oled.drawStr(2, y, b);
    y += 9;
  }
  char vc[MAX_CHANNELS + 1];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    vc[i] = snap.v_calibrated[i] ? '1' : '0';
  }
  vc[MAX_CHANNELS] = 0;
  snprintf(b, sizeof(b), "vcal %s", vc);
  s_oled.drawStr(2, y, b);
  y += 9;
  snprintf(b, sizeof(b), "SD %s", snap.sd_ok ? "mounted" : "missing");
  s_oled.drawStr(2, y, b);
  y += 9;
  snprintf(b, sizeof(b), "HEAP %u k", (unsigned)(snap.free_heap / 1024));
  s_oled.drawStr(2, y, b);
}

static void renderFw() {
  drawHeader("Firmware Info", 0);
  s_oled.setFont(F_SMALL);
  char b[40];
  int y = 22;
  s_oled.drawStr(2, y, "MPS-505 motor protector");
  y += 10;
  snprintf(b, sizeof(b), "Build %s", __DATE__);
  s_oled.drawStr(2, y, b);
  y += 10;
  snprintf(b, sizeof(b), "Time  %s", __TIME__);
  s_oled.drawStr(2, y, b);
  y += 10;
  const uint32_t up = millis() / 1000;
  snprintf(b, sizeof(b), "Uptime %luh %lum %lus",
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
           (unsigned long)(up % 60));
  s_oled.drawStr(2, y, b);
}

static void renderNet() {
  drawHeader("Network Info", 0);
  s_oled.setFont(F_SMALL);
  char b[44];
  int y = 22;
  snprintf(b, sizeof(b), "SSID %s", AP_SSID);
  s_oled.drawStr(2, y, b);
  y += 10;
  snprintf(b, sizeof(b), "Pass %s", AP_PASS);
  s_oled.drawStr(2, y, b);
  y += 10;
  snprintf(b, sizeof(b), "IP   192.168.4.1");
  s_oled.drawStr(2, y, b);
  y += 10;
  snprintf(b, sizeof(b), "User %s", s_net_user);
  s_oled.drawStr(2, y, b);
  y += 10;
  snprintf(b, sizeof(b), "Pass %s", s_net_pass);
  s_oled.drawStr(2, y, b);
}

static void renderScreen() {
  s_oled.clearBuffer();
  if (s_screen == SC_SPLASH) {
    renderSplash();
  } else {
    StatusSnapshot snap;
    protectionSnapshot(&snap);
    updateStatusLed(snap);
    switch (s_screen) {
      case SC_HOME:     renderHome(snap); break;
      case SC_MOTOR:    renderMotor(snap); break;
      case SC_FAULTLOG: renderFaultLog(); break;
      case SC_DIAG:     renderDiag(snap); break;
      case SC_FW:       renderFw(); break;
      case SC_NET:      renderNet(); break;
      default: break;
    }
    if (s_msg[0] && (int32_t)(millis() - s_msg_until) < 0) {
      s_oled.setFont(F_SMALL);
      s_oled.setDrawColor(0);
      s_oled.drawBox(0, 53, 128, 11);
      s_oled.setDrawColor(1);
      s_oled.drawStr(2, 61, s_msg);
    } else if (s_msg[0]) {
      s_msg[0] = 0;
    }
  }
  i2cLock();
  s_oled.sendBuffer();
  i2cUnlock();
}

// --- input ------------------------------------------------------------------

static int pollEvents() {
  const uint32_t now = millis();
  const uint8_t st = (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
  if (st != s_enc_prev) {
    const int idx = ((s_enc_prev << 2) | st) & 0x0F;
    s_enc_acc += kQTable[idx];
    s_enc_prev = st;
    if (s_enc_acc >= 4) {
      s_enc_acc = 0;
      return EV_CW;
    }
    if (s_enc_acc <= -4) {
      s_enc_acc = 0;
      return EV_CCW;
    }
  }
  const uint8_t raw = (uint8_t)digitalRead(ENC_SW_PIN);
  if (raw != s_sw_last) {
    s_sw_last = raw;
    s_sw_change_ms = now;
  }
  if ((uint32_t)(now - s_sw_change_ms) >= UI_SW_DEBOUNCE_MS && raw != s_sw_stable) {
    s_sw_stable = raw;
    if (s_sw_stable == 0) {
      s_pressed = true;
      s_long_fired = false;
      s_press_ms = now;
    } else {
      s_pressed = false;
      if (!s_long_fired) {
        return EV_SHORT;
      }
    }
  }
  if (s_pressed && !s_long_fired && (uint32_t)(now - s_press_ms) >= UI_LONG_MS) {
    s_long_fired = true;
    return EV_LONG;
  }
  return EV_NONE;
}

static void toggleMotor() {
  if (s_motor_sel >= s_motor_n) {
    return;
  }
  const int mi = s_motor_order[s_motor_sel];
  StatusSnapshot snap;
  protectionSnapshot(&snap);
  if (!snap.motors[mi].used) {
    return;
  }
  const MotorStatus st = snap.rt[mi].status;
  if (st == MST_RUNNING) {
    if (!protectionPost(CMD_STOP, (uint8_t)mi)) {
      toneFault();
    }
  } else if (st == MST_STOPPED) {
    const char *reason = nullptr;
    if (protectionCanStart(snap, mi, &reason)) {
      if (!protectionPost(CMD_START, (uint8_t)mi)) {
        setMsg("command queue full");
        toneFault();
      }
    } else {
      setMsg(reason ? reason : "cannot start");
      toneFault();
    }
  } else {
    setMsg("fault/cooling: reset from dashboard");
    toneFault();
  }
}

static void rotate(int dir) {
  switch (s_screen) {
    case SC_HOME: {
      const int rows = s_motor_n + 4;
      s_home_sel += dir;
      if (s_home_sel < 0) {
        s_home_sel = 0;
      }
      if (s_home_sel >= rows) {
        s_home_sel = rows - 1;
      }
      break;
    }
    case SC_MOTOR:
      if (s_motor_n > 0) {
        s_motor_sel += dir;
        if (s_motor_sel < 0) {
          s_motor_sel = 0;
        }
        if (s_motor_sel >= s_motor_n) {
          s_motor_sel = s_motor_n - 1;
        }
      }
      break;
    case SC_FAULTLOG:
      s_fault_sel += dir;
      if (s_fault_sel < 0) {
        s_fault_sel = 0;
      }
      break;
    default:
      break;
  }
  toneClick();
}

static void shortPress() {
  if (s_screen == SC_HOME) {
    if (s_home_sel < s_motor_n) {
      s_motor_sel = s_home_sel;
      s_screen = SC_MOTOR;
    } else {
      const int menu = s_home_sel - s_motor_n;
      if (menu == 0) {
        s_fault_sel = 0;
        s_fault_first = 0;
        s_screen = SC_FAULTLOG;
      } else if (menu == 1) {
        s_screen = SC_DIAG;
      } else if (menu == 2) {
        s_screen = SC_FW;
      } else {
        loadNet();
        s_screen = SC_NET;
      }
    }
    toneClick();
  } else if (s_screen == SC_MOTOR) {
    toggleMotor();
  }
}

static void longPress() {
  if (s_screen == SC_HOME) {
    return;
  }
  s_screen = SC_HOME;
  s_home_first = 0;
  toneBack();
}

static void handleEvent(int ev) {
  if (ev == EV_NONE || s_boot_stage != UI_BOOT_DONE) {
    return;
  }
  if (ev == EV_CW || ev == EV_CCW) {
    rotate(ev == EV_CW ? 1 : -1);
  } else if (ev == EV_SHORT) {
    shortPress();
  } else if (ev == EV_LONG) {
    longPress();
  }
}

// --- public API -------------------------------------------------------------

void uiBootStage(UiBootStage stage) {
  if (stage > UI_BOOT_DONE) {
    stage = UI_BOOT_DONE;
  }
  s_boot_stage = (uint8_t)stage;
  if (stage == UI_BOOT_DONE) {
    s_boot_done_ms = millis();
  }
}

void uiBootNote(const char *text) {
  if (!text) {
    s_boot_note[0] = 0;
    return;
  }
  strncpy(s_boot_note, text, sizeof(s_boot_note) - 1);
  s_boot_note[sizeof(s_boot_note) - 1] = 0;
}

void uiBegin() {
  pinMode(ENC_A_PIN, INPUT);
  pinMode(ENC_B_PIN, INPUT);
  pinMode(ENC_SW_PIN, INPUT);
  s_enc_prev = (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
  s_sw_last = (uint8_t)digitalRead(ENC_SW_PIN);
  s_sw_stable = s_sw_last;

  s_led.begin();
  s_led.setBrightness(40);
  s_led.setPixelColor(0, 0);
  s_led.show();

  i2cLock();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(OLED_I2C_HZ);
  s_oled.begin();
  // U8g2's hardware-I2C setup may call Wire.begin() with no pins; re-bind ours.
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(OLED_I2C_HZ);
  s_oled.setBusClock(OLED_I2C_HZ);
  i2cUnlock();
  s_oled.clearBuffer();
  i2cLock();
  s_oled.sendBuffer();
  i2cUnlock();

  s_ui_start_ms = millis();
  s_boot_done_ms = s_ui_start_ms;
  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 2, nullptr, 0);
}

void uiTask(void *arg) {
  (void)arg;
  uint32_t render_last = 0;
  uint32_t icon_last = 0;
  for (;;) {
    handleEvent(pollEvents());
    const uint32_t now = millis();
    if (s_screen == SC_SPLASH && s_boot_stage == UI_BOOT_DONE &&
        (uint32_t)(now - s_boot_done_ms) >= 1200) {
      rebuildMotorOrder();
      s_screen = SC_HOME;
      render_last = 0;
    }
    if ((uint32_t)(now - icon_last) >= UI_ICON_MS) {
      icon_last = now;
      s_icon_frame = (uint8_t)((s_icon_frame + 1) % 3);
    }
    if ((uint32_t)(now - render_last) >= UI_REFRESH_MS) {
      render_last = now;
      renderScreen();
    }
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}
