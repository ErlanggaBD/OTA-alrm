#include "net_manager.h"
#include "app_config.h"
#include "app_state.h"
#include "log_buffer.h"

#include <HTTPClient.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

static uint32_t s_lastReconnect = 0;
static uint32_t s_lastInternetCheck = 0;
static bool s_internetKnown = false;
static bool s_internetOk = false;

static NetworkClient* makeClient(WiFiClient& plain, WiFiClientSecure& secure, const String& url) {
  if (url.startsWith("https://")) {
#if ALLOW_INSECURE_TLS
    secure.setInsecure();
#else
    if (strlen(API_ROOT_CA) > 0) {
      secure.setCACert(API_ROOT_CA);
    } else {
      secure.setInsecure();
    }
#endif
    return &secure;
  }
  return &plain;
}

void netBegin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  setenv("TZ", "WIB-7", 1);
  tzset();
}

bool netConnectTo(const char* ssid, const char* pass, uint32_t timeoutMs) {
  if (!ssid || strlen(ssid) == 0) {
    logAdd("warn", "SSID kosong, tidak bisa konek");
    return false;
  }

  if (g_ap_mode) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }

  WiFi.begin(ssid, pass ? pass : "");
  logAdd("info", "konek Wi-Fi ke %s", ssid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logAdd("info", "Wi-Fi terhubung, IP %s, RSSI %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    netSyncTime();
    return true;
  }

  logAdd("error", "gagal konek Wi-Fi (status %d)", (int)WiFi.status());
  WiFi.disconnect(false, false);
  if (g_ap_mode) {
    // Keep the captive portal alive for another attempt.
    WiFi.mode(WIFI_AP);
  }
  return false;
}

bool netConnectSta(uint32_t timeoutMs) {
  return netConnectTo(g_config.ssid, g_config.wifi_pass, timeoutMs);
}

void netLoop() {
  if (g_ap_mode) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - s_lastReconnect < WIFI_RECONNECT_INTERVAL_MS) return;

  s_lastReconnect = millis();
  logAdd("warn", "Wi-Fi terputus, mencoba reconnect");
  WiFi.reconnect();
}

bool netIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

int netRssi() {
  return netIsConnected() ? WiFi.RSSI() : 0;
}

String netIp() {
  return netIsConnected() ? WiFi.localIP().toString() : String("0.0.0.0");
}

String netSsid() {
  if (netIsConnected()) {
    String ssid = WiFi.SSID();
    if (ssid.length()) return ssid;
  }
  return String(g_config.ssid);
}

void netSyncTime() {
  if (!netIsConnected()) return;
  if (timeIsValid()) return;

  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (!timeIsValid() && (millis() - start) < NTP_WAIT_MS) {
    delay(200);
  }
  if (timeIsValid()) {
    logAdd("info", "NTP sinkron, epoch %lu", (unsigned long)nowEpoch());
  } else {
    logAdd("warn", "NTP belum tersedia");
  }
}

bool netCheckInternet() {
  if (!netIsConnected()) return false;
  // Cache the result for 30 seconds to avoid hammering the vendor server.
  if (s_internetKnown && millis() - s_lastInternetCheck < 30000UL) {
    return s_internetOk;
  }

  String url = String(g_config.api_base) + "/api/v1/health";
  WiFiClient plain;
  WiFiClientSecure secure;
  NetworkClient* client = makeClient(plain, secure, url);

  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(4000);
  if (!http.begin(*client, url)) {
    s_internetKnown = true;
    s_lastInternetCheck = millis();
    s_internetOk = false;
    return false;
  }
  int code = http.GET();
  http.end();
  s_internetKnown = true;
  s_lastInternetCheck = millis();
  s_internetOk = (code == 200);
  return s_internetOk;
}
