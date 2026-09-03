#include <Arduino.h>
#include <WiFi.h>
#include "net_ap.h"
#include "config_limits.h"

void netApBegin() {
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  WiFi.mode(WIFI_AP);
  const bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  if (!ok) {
    Serial.println("SoftAP: start failed");
  }
}

void netApPrintBanner(const char *user, const char *pass) {
  const IPAddress ip = WiFi.softAPIP();
  Serial.println("---- MPS-505 boot ----");
  Serial.println("mode: SoftAP");
  Serial.printf("SSID: %s\n", WiFi.softAPSSID().c_str());
  Serial.printf("pass: %s\n", AP_PASS);
  Serial.printf("IP:   %s\n", ip.toString().c_str());
  Serial.printf("dash user: %s\n", user);
  Serial.printf("dash pass: %s\n", pass);
  Serial.println("----------------------");
}
