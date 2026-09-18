#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <string.h>
#include <stdlib.h>

#include "web.h"
#include "web_html.h"
#include "motor_store.h"
#include "protection.h"
#include "sd_log.h"
#include "buzzer.h"
#include "config_limits.h"

static AsyncWebServer s_server(80);
static char s_json[8192];
static SemaphoreHandle_t s_json_mu;
static char s_sess[33];
static uint32_t s_sess_exp_ms;
static const uint32_t SESS_TTL_MS = 8ul * 3600ul * 1000ul;

static void jsonLock() {
  xSemaphoreTake(s_json_mu, portMAX_DELAY);
}

static void jsonUnlock() {
  xSemaphoreGive(s_json_mu);
}

static bool timingEq(const char *a, const char *b) {
  size_t n = strlen(a);
  if (n != strlen(b)) {
    return false;
  }
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) {
    d |= (uint8_t)a[i] ^ (uint8_t)b[i];
  }
  return d == 0;
}

static bool readCookie(AsyncWebServerRequest *req, const char *name, char *out, size_t out_len) {
  if (!req->hasHeader("Cookie")) {
    return false;
  }
  const String &ck = req->getHeader("Cookie")->value();
  const size_t nlen = strlen(name);
  const char *s = ck.c_str();
  while (*s) {
    while (*s == ' ') {
      s++;
    }
    if (strncmp(s, name, nlen) == 0 && s[nlen] == '=') {
      s += nlen + 1;
      size_t i = 0;
      while (*s && *s != ';' && i + 1 < out_len) {
        out[i++] = *s++;
      }
      out[i] = 0;
      return i > 0;
    }
    while (*s && *s != ';') {
      s++;
    }
    if (*s == ';') {
      s++;
    }
  }
  return false;
}

static void clearSession() {
  s_sess[0] = 0;
  s_sess_exp_ms = 0;
}

static void mintSession() {
  snprintf(s_sess, sizeof(s_sess), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
  s_sess_exp_ms = millis() + SESS_TTL_MS;
}

static bool sessionOk(AsyncWebServerRequest *req) {
  if (!s_sess[0]) {
    return false;
  }
  if ((int32_t)(millis() - s_sess_exp_ms) >= 0) {
    clearSession();
    return false;
  }
  char tok[33];
  if (!readCookie(req, "mps_sess", tok, sizeof(tok))) {
    return false;
  }
  return timingEq(tok, s_sess);
}

static void sendLoginRedirect(AsyncWebServerRequest *req) {
  AsyncWebServerResponse *r = req->beginResponse(302, "text/plain", "");
  r->addHeader("Location", "/login");
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

static bool auth(AsyncWebServerRequest *req) {
  if (sessionOk(req)) {
    return true;
  }
  const bool api = strncmp(req->url().c_str(), "/api/", 5) == 0;
  if (api) {
    req->send(401, "application/json", "{\"ok\":0,\"err\":\"unauthorized\"}");
  } else {
    sendLoginRedirect(req);
  }
  return false;
}

static void logHeap(const char *tag) {
  Serial.printf("HEAP: web %s=%u\n", tag, (unsigned)ESP.getFreeHeap());
}

static void copyParam(AsyncWebServerRequest *req, const char *name, char *out, size_t out_len, const char *def = "") {
  const AsyncWebParameter *p = nullptr;
  if (req->hasParam(name, true)) {
    p = req->getParam(name, true);
  } else if (req->hasParam(name, false)) {
    p = req->getParam(name, false);
  }
  if (p) {
    strncpy(out, p->value().c_str(), out_len - 1);
  } else {
    strncpy(out, def, out_len - 1);
  }
  out[out_len - 1] = 0;
}

static float paramF(AsyncWebServerRequest *req, const char *name, const char *def = "0") {
  char buf[32];
  copyParam(req, name, buf, sizeof(buf), def);
  return strtof(buf, nullptr);
}

static int paramI(AsyncWebServerRequest *req, const char *name, const char *def = "0") {
  char buf[16];
  copyParam(req, name, buf, sizeof(buf), def);
  return atoi(buf);
}

static void sendJson(AsyncWebServerRequest *req, int code, const char *body) {
  req->send(code, "application/json", body);
  logHeap("resp");
}

static void sendJsonLocked(AsyncWebServerRequest *req, int code) {
  String body(s_json);
  jsonUnlock();
  req->send(code, "application/json", body);
  logHeap("resp");
}

static void sendErr(AsyncWebServerRequest *req, const char *err) {
  char esc[96];
  size_t o = 0;
  for (size_t i = 0; err && err[i] && o + 2 < sizeof(esc); i++) {
    if (err[i] == '"' || err[i] == '\\') {
      if (o + 3 >= sizeof(esc)) {
        break;
      }
      esc[o++] = '\\';
    }
    if ((unsigned char)err[i] < 32) {
      continue;
    }
    if (err[i] == '\n' || err[i] == '\r') {
      continue;
    }
    esc[o++] = err[i];
  }
  esc[o] = 0;
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"ok\":0,\"err\":\"%s\"}", esc);
  sendJson(req, 200, buf);
}

static void sendOk(AsyncWebServerRequest *req) {
  sendJson(req, 200, "{\"ok\":1}");
}

static bool fillFromReq(AsyncWebServerRequest *req, MotorRecord *m) {
  memset(m, 0, sizeof(*m));
  for (int p = 0; p < MAX_PHASES; p++) {
    m->channels[p] = CH_UNUSED;
    m->relay_active_high[p] = 1;
  }
  copyParam(req, "name", m->name, NAME_LEN, "");
  m->phase_count = (uint8_t)paramI(req, "phases", "1");
  m->is_ac = (uint8_t)paramI(req, "ac", "1");
  m->mains_hz = (uint8_t)paramI(req, "hz", "50");
  m->in_amps = paramF(req, "in", "0");
  m->stall_amps = paramF(req, "stall", "0");
  m->cooling_s = paramF(req, "cool", "0");
  m->auto_restart = (uint8_t)paramI(req, "auto", "0");
  m->rated_ac_v = paramF(req, "vac", "0");
  m->uv_volts = paramF(req, "uv", "0");
  m->ov_volts = paramF(req, "ov", "0");
  const uint8_t pol = (uint8_t)paramI(req, "pol", "1");
  for (int p = 0; p < MAX_PHASES; p++) {
    m->relay_active_high[p] = pol;
  }
  m->step_count = (uint8_t)paramI(req, "n", "0");
  if (m->step_count > MAX_STEPS) {
    m->step_count = MAX_STEPS;
  }
  char kn[8];
  char tn[8];
  for (int i = 0; i < m->step_count; i++) {
    snprintf(kn, sizeof(kn), "k%d", i);
    snprintf(tn, sizeof(tn), "t%d", i);
    m->step_k[i] = paramF(req, kn, "0");
    m->step_t_s[i] = paramF(req, tn, "0");
  }
  return true;
}

static const char *stName(MotorStatus s) {
  switch (s) {
    case MST_RUNNING: return "Running";
    case MST_FAULT:   return "Fault";
    case MST_COOLING: return "Cooling";
    default:          return "Stopped";
  }
}

static const char *ftName(FaultType t) {
  switch (t) {
    case FT_I2T:       return "I2T";
    case FT_STALL:     return "STALL";
    case FT_SENSOR:    return "SENSOR_FAULT";
    case FT_UNDERVOLT: return "UNDERVOLT";
    case FT_OVERVOLT:  return "OVERVOLT";
    case FT_NO_CURRENT: return "NO_CURRENT";
    default:          return "";
  }
}

static void handleStatus(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  StatusSnapshot snap;
  protectionSnapshot(&snap);
  jsonLock();
  char *w = s_json;
  char *end = s_json + sizeof(s_json);
  int n = snprintf(w, end - w,
                   "{\"ok\":1,\"sd_ok\":%u,\"ads_ok\":[%u,%u],\"heap\":%u,\"calibrated\":%u,\"free_ch\":%d,\"motors\":[",
                   (unsigned)snap.sd_ok, (unsigned)snap.ads_ok[0], (unsigned)snap.ads_ok[1],
                   (unsigned)snap.free_heap, (unsigned)snap.calibrated,
                   motorStoreFreeCount());
  if (n < 0 || w + n >= end) {
    jsonUnlock();
    sendErr(req, "json overflow");
    return;
  }
  w += n;
  bool first = true;
  for (int i = 0; i < MAX_MOTORS; i++) {
    const MotorRecord *m = &snap.motors[i];
    if (!m->used) {
      continue;
    }
    if (!first) {
      if (w + 2 >= end) {
        jsonUnlock();
        sendErr(req, "json overflow");
        return;
      }
      *w++ = ',';
    }
    first = false;
    char chbuf[24];
    char *cw = chbuf;
    *cw = 0;
    for (int p = 0; p < m->phase_count; p++) {
      cw += snprintf(cw, chbuf + sizeof(chbuf) - cw, "%s%u", p ? "," : "", (unsigned)m->channels[p]);
    }
    char rmsbuf[48];
    char *rw = rmsbuf;
    *rw = 0;
    for (int p = 0; p < m->phase_count; p++) {
      const uint8_t c = m->channels[p];
      const float r = (c < MAX_CHANNELS) ? snap.rms[c] : 0;
      rw += snprintf(rw, rmsbuf + sizeof(rmsbuf) - rw, "%s%.3f", p ? "," : "", (double)r);
    }
    char vbuf[48];
    char *vw = vbuf;
    *vw = 0;
    uint8_t vcal = 1;
    for (int p = 0; p < m->phase_count; p++) {
      const uint8_t c = m->channels[p];
      const float v = (c < MAX_CHANNELS) ? snap.volts[c] : 0;
      vw += snprintf(vw, vbuf + sizeof(vbuf) - vw, "%s%.3f", p ? "," : "", (double)v);
      if (c >= MAX_CHANNELS || !snap.v_calibrated[c]) {
        vcal = 0;
      }
    }
    char pwr[16];
    if (m->is_ac) {
      snprintf(pwr, sizeof(pwr), "%.0f", (double)snap.rt[i].power);
    } else {
      snprintf(pwr, sizeof(pwr), "%.1f", (double)snap.rt[i].power);
    }
    char stepbuf[384];
    char *sw = stepbuf;
    *sw++ = '[';
    *sw = 0;
    for (int s = 0; s < m->step_count; s++) {
      sw += snprintf(sw, stepbuf + sizeof(stepbuf) - sw, "%s{\"k\":%.4f,\"t\":%.3f}",
                     s ? "," : "", (double)m->step_k[s], (double)m->step_t_s[s]);
    }
    if (sw < stepbuf + sizeof(stepbuf) - 1) {
      *sw++ = ']';
      *sw = 0;
    }
    n = snprintf(w, end - w,
                 "{\"idx\":%d,\"used\":1,\"name\":\"%s\",\"status\":%u,\"status_s\":\"%s\","
                 "\"uptime_ms\":%u,\"fault_count\":%u,\"last_fault\":\"%s\",\"channels\":[%s],"
                 "\"phases\":%u,\"rms\":[%s],\"volts\":[%s],\"thermal_pct\":%.1f,"
                 "\"power\":%s,\"power_unit\":\"%s\",\"energy\":%.2f,"
                 "\"in\":%.4f,\"stall\":%.4f,\"cool\":%.3f,\"ac\":%u,\"hz\":%u,"
                 "\"vac\":%.3f,\"uv\":%.3f,\"ov\":%.3f,\"auto\":%u,\"pol\":%u,\"vcal\":%u,\"steps\":%s}",
                 i, m->name, (unsigned)snap.rt[i].status, stName(snap.rt[i].status),
                 (unsigned)snap.rt[i].run_start_ms, (unsigned)snap.rt[i].fault_count,
                 ftName(snap.rt[i].last_fault), chbuf, (unsigned)m->phase_count,
                 rmsbuf, vbuf, (double)snap.thermal_pct[i],
                 pwr, m->is_ac ? "VA" : "W", (double)snap.rt[i].energy,
                 (double)m->in_amps, (double)m->stall_amps, (double)m->cooling_s,
                 (unsigned)m->is_ac, (unsigned)m->mains_hz,
                 (double)m->rated_ac_v, (double)m->uv_volts, (double)m->ov_volts,
                 (unsigned)m->auto_restart, (unsigned)m->relay_active_high[0],
                 (unsigned)vcal, stepbuf);
    if (n < 0 || w + n >= end) {
      jsonUnlock();
      sendErr(req, "json overflow");
      return;
    }
    w += n;
  }
  if (w + 3 >= end) {
    jsonUnlock();
    sendErr(req, "json overflow");
    return;
  }
  *w++ = ']';
  *w++ = '}';
  *w = 0;
  sendJsonLocked(req, 200);
}

static void handleCmd(AsyncWebServerRequest *req, CmdType t) {
  if (!auth(req)) {
    return;
  }
  const int idx = paramI(req, "idx", "-1");
  if (idx < 0 || idx >= MAX_MOTORS) {
    sendErr(req, "bad idx");
    return;
  }
  if (t == CMD_START) {
    StatusSnapshot snap;
    protectionSnapshot(&snap);
    if (snap.motors[idx].used && !snap.motors[idx].is_ac) {
      bool ads_ok = true;
      bool vcal = true;
      for (int p = 0; p < snap.motors[idx].phase_count; p++) {
        const uint8_t c = snap.motors[idx].channels[p];
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
      if (!ads_ok) {
        sendErr(req, "DC start needs the ADS1115 for this motor's channels");
        return;
      }
      if (!vcal) {
        sendErr(req, "calibrate DC voltage zeros first");
        return;
      }
    }
  }
  if (!protectionPost(t, (uint8_t)idx)) {
    sendErr(req, "queue full");
    return;
  }
  toneClick();
  sendOk(req);
}

static void handleCal(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  if (!protectionAllIdleForCal()) {
    sendErr(req, "calibrate only when all motors are Stopped or Fault");
    return;
  }
  if (!protectionPost(CMD_CALIBRATE, 0)) {
    sendErr(req, "queue full");
    return;
  }
  sendOk(req);
}

static void handleAlloc(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  const int n = paramI(req, "n", "1");
  uint8_t ch[MAX_PHASES];
  if (n != 1 && n != 3) {
    sendErr(req, "n must be 1 or 3");
    return;
  }
  if (!motorStoreAlloc((uint8_t)n, ch)) {
    sendErr(req, "not enough free channels");
    return;
  }
  jsonLock();
  if (n == 1) {
    snprintf(s_json, sizeof(s_json), "{\"ok\":1,\"channels\":[%u]}", (unsigned)ch[0]);
  } else {
    snprintf(s_json, sizeof(s_json), "{\"ok\":1,\"channels\":[%u,%u,%u]}",
             (unsigned)ch[0], (unsigned)ch[1], (unsigned)ch[2]);
  }
  sendJsonLocked(req, 200);
}

static void handleAdd(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  MotorRecord rec;
  char err[64];
  fillFromReq(req, &rec);
  const int slot = motorStoreAdd(&rec, err, sizeof(err));
  if (slot < 0) {
    sendErr(req, err);
    return;
  }
  protectionPost(CMD_RELOAD, 0);
  toneMotorAdded();
  jsonLock();
  snprintf(s_json, sizeof(s_json), "{\"ok\":1,\"idx\":%d}", slot);
  sendJsonLocked(req, 200);
}

static void handleEdit(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  const int idx = paramI(req, "idx", "-1");
  MotorRecord rec;
  char err[64];
  fillFromReq(req, &rec);
  if (!motorStoreEdit(idx, &rec, err, sizeof(err))) {
    sendErr(req, err);
    return;
  }
  protectionPost(CMD_RELOAD, 0);
  sendOk(req);
}

static void handleDel(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  const int idx = paramI(req, "idx", "-1");
  char err[64];
  if (!motorStoreDelete(idx, protectionMotorStatus(idx), err, sizeof(err))) {
    sendErr(req, err);
    return;
  }
  protectionPost(CMD_RELOAD, 0);
  sendOk(req);
}

static void handleLog(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  if (!sdLogOk()) {
    sendJson(req, 200, "{\"ok\":1,\"sd_ok\":0,\"rows\":[]}");
    return;
  }
  char csv[4096];
  size_t n = 0;
  if (!sdLogReadAll(csv, sizeof(csv), &n)) {
    sendJson(req, 200, "{\"ok\":1,\"sd_ok\":1,\"rows\":[]}");
    return;
  }
  jsonLock();
  char *w = s_json;
  char *end = s_json + sizeof(s_json);
  int k = snprintf(w, end - w, "{\"ok\":1,\"sd_ok\":1,\"rows\":[");
  w += k;
  bool first = true;
  char *save = nullptr;
  char *line = strtok_r(csv, "\n", &save);
  while (line) {
    if (strncmp(line, "uptime_ms", 9) == 0) {
      line = strtok_r(nullptr, "\n", &save);
      continue;
    }
    if (line[0] == 0 || line[0] == '\r') {
      line = strtok_r(nullptr, "\n", &save);
      continue;
    }
    char fields[7][24];
    for (int fi = 0; fi < 7; fi++) {
      fields[fi][0] = 0;
    }
    {
      char *fp = line;
      for (int fi = 0; fi < 7 && fp; fi++) {
        char *comma = strchr(fp, ',');
        if (comma) {
          *comma = 0;
        }
        strncpy(fields[fi], fp, sizeof(fields[fi]) - 1);
        fields[fi][sizeof(fields[fi]) - 1] = 0;
        fp = comma ? comma + 1 : nullptr;
      }
    }
    if (!first) {
      if (w + 2 < end) {
        *w++ = ',';
      }
    }
    first = false;
    k = snprintf(w, end - w,
                 "{\"uptime_ms\":\"%s\",\"motor\":\"%s\",\"type\":\"%s\",\"current_A\":\"%s\","
                 "\"voltage_V\":\"%s\",\"power_W\":\"%s\",\"power_VA\":\"%s\"}",
                 fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6]);
    if (k < 0 || w + k >= end) {
      break;
    }
    w += k;
    line = strtok_r(nullptr, "\n", &save);
  }
  if (w + 3 < end) {
    *w++ = ']';
    *w++ = '}';
    *w = 0;
  } else {
    jsonUnlock();
    sendErr(req, "json overflow");
    return;
  }
  sendJsonLocked(req, 200);
}

static void handleLogExport(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  if (!sdLogOk()) {
    req->send(503, "text/plain", "SD unavailable");
    return;
  }
  req->send(SD, FAULT_LOG_PATH, "text/csv", true);
}

static void handleLogClear(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  if (!sdLogClear()) {
    sendErr(req, "SD unavailable");
    return;
  }
  sendOk(req);
}

static void handleAuthSet(AsyncWebServerRequest *req) {
  if (!auth(req)) {
    return;
  }
  char u[AUTH_USER_LEN];
  char p[AUTH_PASS_LEN];
  copyParam(req, "user", u, sizeof(u), "");
  copyParam(req, "pass", p, sizeof(p), "");
  if (!motorStoreAuthSet(u, p)) {
    sendErr(req, "user and password required");
    return;
  }
  clearSession();
  sendOk(req);
}

static void handleLoginGet(AsyncWebServerRequest *req) {
  if (sessionOk(req)) {
    AsyncWebServerResponse *r = req->beginResponse(302, "text/plain", "");
    r->addHeader("Location", "/");
    req->send(r);
    return;
  }
  req->send_P(200, "text/html", LOGIN_HTML);
}

static void handleLoginPost(AsyncWebServerRequest *req) {
  char u[AUTH_USER_LEN];
  char p[AUTH_PASS_LEN];
  char got_u[AUTH_USER_LEN];
  char got_p[AUTH_PASS_LEN];
  motorStoreAuthGet(u, p);
  copyParam(req, "user", got_u, sizeof(got_u), "");
  copyParam(req, "pass", got_p, sizeof(got_p), "");
  AsyncWebServerResponse *r;
  if (timingEq(got_u, u) && timingEq(got_p, p)) {
    mintSession();
    char setck[80];
    snprintf(setck, sizeof(setck),
             "mps_sess=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800", s_sess);
    r = req->beginResponse(302, "text/plain", "");
    r->addHeader("Location", "/");
    r->addHeader("Set-Cookie", setck);
  } else {
    r = req->beginResponse(302, "text/plain", "");
    r->addHeader("Location", "/login?bad=1");
  }
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

static void handleLogout(AsyncWebServerRequest *req) {
  clearSession();
  AsyncWebServerResponse *r = req->beginResponse(302, "text/plain", "");
  r->addHeader("Location", "/login");
  r->addHeader("Set-Cookie", "mps_sess=; Path=/; Max-Age=0; HttpOnly");
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void webBegin() {
  s_json_mu = xSemaphoreCreateMutex();
  clearSession();
  s_server.on("/login", HTTP_GET, handleLoginGet);
  s_server.on("/login", HTTP_POST, handleLoginPost);
  s_server.on("/logout", HTTP_GET, handleLogout);
  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!auth(req)) {
      return;
    }
    req->send_P(200, "text/html", INDEX_HTML);
    logHeap("html");
  });
  s_server.on("/api/status", HTTP_GET, handleStatus);
  s_server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *req) { handleCmd(req, CMD_START); });
  s_server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *req) { handleCmd(req, CMD_STOP); });
  s_server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *req) { handleCmd(req, CMD_RESET); });
  s_server.on("/api/calibrate", HTTP_POST, handleCal);
  s_server.on("/api/alloc", HTTP_GET, handleAlloc);
  s_server.on("/api/motor/edit", HTTP_POST, handleEdit);
  s_server.on("/api/motor/del", HTTP_POST, handleDel);
  s_server.on("/api/motor/add", HTTP_POST, handleAdd);
  s_server.on("/api/log", HTTP_GET, handleLog);
  s_server.on("/api/log/export", HTTP_GET, handleLogExport);
  s_server.on("/api/log/clear", HTTP_POST, handleLogClear);
  s_server.on("/api/auth", HTTP_POST, handleAuthSet);
  s_server.onNotFound([](AsyncWebServerRequest *req) {
    if (!auth(req)) {
      return;
    }
    req->send(404, "text/plain", "not found");
  });
  s_server.begin();
}
