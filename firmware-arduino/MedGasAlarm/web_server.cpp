#include "web_server.h"
#include "alarm.h"
#include "app_config.h"
#include "app_state.h"
#include "cloud_sync.h"
#include "log_buffer.h"
#include "net_manager.h"
#include "ota_manager.h"
#include "provisioning.h"
#include "sensors.h"
#include "storage.h"
#include "web_ui.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_system.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
struct Session {
  char token[33];
  uint32_t expiresAt;
  bool active;
};

static WebServer server(80);
static char g_csrf[33];
static Session s_sessions[MAX_SESSIONS];
static bool s_otaRawAccepted = false;
static uint32_t s_factoryResetAt = 0;
static uint8_t s_loginFailures = 0;
static uint32_t s_loginLockedUntil = 0;

static void randomHexToken(char* out, size_t bytes) {
  static const char* hex = "0123456789abcdef";
  for (size_t i = 0; i < bytes; i++) {
    uint8_t v = (uint8_t)(esp_random() & 0xFF);
    out[i * 2] = hex[(v >> 4) & 0x0F];
    out[i * 2 + 1] = hex[v & 0x0F];
  }
  out[bytes * 2] = 0;
}

static bool sessionExpired(const Session& s) {
  return !s.active || (int32_t)(millis() - s.expiresAt) >= 0;
}

static void expireSessions() {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (s_sessions[i].active && sessionExpired(s_sessions[i])) {
      s_sessions[i].active = false;
      s_sessions[i].token[0] = 0;
    }
  }
}

static String cookieValue(const char* name) {
  String cookies = server.header("Cookie");
  if (!cookies.length()) return String();
  String needle = String(name) + "=";
  int start = cookies.indexOf(needle);
  if (start < 0) return String();
  start += needle.length();
  int end = cookies.indexOf(';', start);
  if (end < 0) end = cookies.length();
  String value = cookies.substring(start, end);
  value.trim();
  return value;
}

static int findSession(const String& token) {
  if (!token.length()) return -1;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!s_sessions[i].active) continue;
    if (token.equals(s_sessions[i].token)) {
      if (sessionExpired(s_sessions[i])) {
        s_sessions[i].active = false;
        return -1;
      }
      return i;
    }
  }
  return -1;
}

static bool sessionValid() {
  int idx = findSession(cookieValue("mgsid"));
  if (idx < 0) return false;
  s_sessions[idx].expiresAt = millis() + SESSION_TTL_MS;
  return true;
}

static String createSession() {
  expireSessions();
  int slot = -1;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!s_sessions[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    // Evict the session that expires first.
    slot = 0;
    for (int i = 1; i < MAX_SESSIONS; i++) {
      if ((int32_t)(s_sessions[i].expiresAt - s_sessions[slot].expiresAt) < 0) slot = i;
    }
  }
  randomHexToken(s_sessions[slot].token, 16);
  s_sessions[slot].expiresAt = millis() + SESSION_TTL_MS;
  s_sessions[slot].active = true;
  return String(s_sessions[slot].token);
}

static void destroySession() {
  int idx = findSession(cookieValue("mgsid"));
  if (idx >= 0) {
    s_sessions[idx].active = false;
    s_sessions[idx].token[0] = 0;
  }
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
static void sendJson(int code, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendError(int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message ? message : "error";
  sendJson(code, doc);
}

static void sendOk(const char* message) {
  JsonDocument doc;
  doc["ok"] = true;
  if (message) doc["message"] = message;
  sendJson(200, doc);
}

static bool requireAuth() {
  if (!sessionValid()) {
    sendError(401, "unauthorized");
    return false;
  }
  return true;
}

static bool requireCsrf() {
  if (server.header("X-CSRF-Token") != String(g_csrf)) {
    sendError(403, "CSRF token invalid");
    return false;
  }
  return true;
}

static bool requireUnlocked() {
  if (g_ui_locked) {
    sendError(423, g_lock_reason[0] ? g_lock_reason : "locked");
    return false;
  }
  return true;
}

static void servePage(const char* content, PGM_P contentType, bool cache) {
  server.sendHeader("Cache-Control", cache ? "public, max-age=3600" : "no-store");
  server.send_P(200, contentType, content);
}

static void redirectTo(const String& location) {
  server.sendHeader("Location", location);
  server.send(302, "text/plain", "");
}

static void isoFromEpoch(uint32_t epoch, char* out, size_t len) {
  if (!epoch) {
    if (len) out[0] = 0;
    return;
  }
  time_t t = (time_t)epoch;
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
}

// ---------------------------------------------------------------------------
// Static pages
// ---------------------------------------------------------------------------
static void handleRoot() {
  if (!g_provisioned || g_ap_mode) {
    servePage(SETUP_HTML, PSTR("text/html"), false);
    return;
  }
  servePage(INDEX_HTML, PSTR("text/html"), false);
}

static void handleSetupPage() {
  servePage(SETUP_HTML, PSTR("text/html"), false);
}

static void handleSettingsPage() {
  if (!g_provisioned) {
    redirectTo("/");
    return;
  }
  servePage(SETTINGS_HTML, PSTR("text/html"), false);
}

static void handleLoginPage() {
  servePage(LOGIN_HTML, PSTR("text/html"), false);
}

static void handleLockedPage() {
  servePage(LOCKED_HTML, PSTR("text/html"), false);
}

static void handleCss() {
  servePage(STYLE_CSS, PSTR("text/css"), true);
}

static void handleJs() {
  servePage(APP_JS, PSTR("application/javascript"), true);
}

static void handleFavicon() {
  server.send(204, "text/plain", "");
}

// ---------------------------------------------------------------------------
// GET /api/system
// ---------------------------------------------------------------------------
static void handleSystem() {
  JsonDocument doc;
  doc["provisioned"] = g_provisioned;
  doc["ap_mode"] = g_ap_mode;
  doc["locked"] = g_ui_locked;
  doc["lock_reason"] = g_lock_reason;
  doc["csrf"] = g_csrf;
  doc["ap_ssid"] = g_ap_ssid;
  doc["ap_pass"] = g_ap_pass;

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["serial"] = g_serial;
  dev["hw"] = HW_REV;
  dev["fw"] = FW_VERSION;
  dev["uptime"] = millis() / 1000UL;
  dev["ip"] = netIp();
  dev["rssi"] = netRssi();
  dev["ssid"] = netSsid();

  JsonObject cloud = doc["cloud"].to<JsonObject>();
  cloud["online"] = g_cloud.online;
  cloud["last_sync"] = g_cloud.last_sync;
  cloud["last_error"] = g_cloud.last_error;

  JsonObject alarm = doc["alarm"].to<JsonObject>();
  alarm["state"] = alarmLevelName(g_alarm.state);
  alarm["latched"] = g_alarm.latched;
  alarm["acknowledged"] = g_alarm.acknowledged;
  alarm["summary"] = g_alarm.summary;

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  JsonObject ota = doc["ota"].to<JsonObject>();

  configLock(portMAX_DELAY);
  dev["email"] = g_config.email;
  cloud["plan"] = g_config.plan;
  cloud["quota_expires"] = g_config.quota_expires;
  cloud["policy"] = lockPolicyName(g_config.lock_policy);
  cloud["grace_days"] = g_config.grace_days;
  cloud["offline_action"] = offlineActionName(g_config.offline_action);
  sensors["active"] = g_config.active_sensors;
  sensors["total"] = SENSOR_COUNT;
  ota["available"] = strlen(g_config.fw_url) > 0;
  ota["version"] = g_config.fw_available_version;
  ota["size"] = g_config.fw_size;

  JsonArray triggered = alarm["triggered"].to<JsonArray>();
  for (uint8_t i = 0; i < g_config.active_sensors; i++) {
    if (!(g_alarm.active_mask & (1UL << i))) continue;
    JsonObject t = triggered.add<JsonObject>();
    t["idx"] = i;
    t["label"] = g_config.labels[i];
  }
  configUnlock();

  ota["status"] = g_ota_status;
  ota["progress"] = g_ota_progress;
  ota["error"] = g_ota_error;
  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// GET /api/sensors
// ---------------------------------------------------------------------------
static void handleSensors() {
  if (!requireAuth()) return;
  if (!requireUnlocked()) return;

  JsonDocument doc;
  JsonArray arr = doc["sensors"].to<JsonArray>();

  configLock(portMAX_DELAY);
  uint8_t active = g_config.active_sensors;
  for (uint8_t i = 0; i < active && i < SENSOR_COUNT; i++) {
    JsonObject o = arr.add<JsonObject>();
    const SensorState& s = g_sensors[i];
    o["idx"] = i;
    o["label"] = g_config.labels[i];
    o["unit"] = g_config.thresholds[i].unit;
    o["secondary_unit"] = "%";
    o["valid"] = s.valid;
    o["fault"] = (s.state == ALARM_FAULT);
    o["state"] = alarmLevelName(s.state);
    o["age"] = sensorAgeSeconds(i);
    o["last_read"] = s.last_read_epoch;
    if (s.valid && !isnan(s.primary)) o["value"] = s.primary;
    else o["value"] = nullptr;
    if (s.valid && !isnan(s.secondary)) o["secondary"] = s.secondary;
    else o["secondary"] = nullptr;
  }
  configUnlock();

  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// /api/config
// ---------------------------------------------------------------------------
static void handleConfigGet() {
  if (!requireAuth()) return;
  if (!requireUnlocked()) return;

  JsonDocument doc;
  configLock(portMAX_DELAY);
  doc["active_sensors"] = g_config.active_sensors;
  doc["default_active_sensors"] = g_config.default_active_sensors;
  doc["lock_policy"] = lockPolicyName(g_config.lock_policy);
  doc["allow_local_override"] = g_config.allow_local_override;
  doc["grace_days"] = g_config.grace_days;
  doc["offline_action"] = offlineActionName(g_config.offline_action);

  JsonArray labels = doc["sensor_labels"].to<JsonArray>();
  JsonArray thresholds = doc["thresholds"].to<JsonArray>();
  for (int i = 0; i < SENSOR_COUNT; i++) {
    labels.add(g_config.labels[i]);
    JsonObject t = thresholds.add<JsonObject>();
    t["idx"] = i;
    t["warn_low"] = g_config.thresholds[i].warn_low;
    t["alarm_low"] = g_config.thresholds[i].alarm_low;
    t["warn_high"] = g_config.thresholds[i].warn_high;
    t["alarm_high"] = g_config.thresholds[i].alarm_high;
    t["hysteresis"] = g_config.thresholds[i].hysteresis;
    t["unit"] = g_config.thresholds[i].unit;
  }

  JsonObject quota = doc["quota"].to<JsonObject>();
  quota["plan"] = g_config.plan;
  quota["expires_epoch"] = g_config.quota_expires;
  char iso[24];
  isoFromEpoch(g_config.quota_expires, iso, sizeof(iso));
  quota["expires_at"] = iso;

  JsonObject fw = doc["firmware"].to<JsonObject>();
  fw["version"] = g_config.fw_available_version;
  fw["url"] = g_config.fw_url;
  fw["sha256"] = g_config.fw_sha;
  fw["size"] = g_config.fw_size;
  configUnlock();

  doc["server_time"] = nowEpoch();
  doc["issued_at"] = nowEpoch();
  doc["expires_at"] = nowEpoch() + 86400UL;
  sendJson(200, doc);
}

static void handleConfigPost() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireUnlocked()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    sendError(400, "JSON tidak valid");
    return;
  }

  bool changed = false;

  if (!doc["lock_policy"].isNull()) {
    if (!g_config.allow_local_override) {
      sendError(403, "Perubahan lock policy dikunci oleh vendor");
      return;
    }
    const char* policy = doc["lock_policy"] | "";
    uint8_t next = g_config.lock_policy;
    if (strcmp(policy, "provision_only") == 0) next = LOCK_PROVISION_ONLY;
    else if (strcmp(policy, "grace") == 0) next = LOCK_GRACE;
    else if (strcmp(policy, "strict") == 0) next = LOCK_STRICT;
    else {
      sendError(400, "lock_policy tidak valid");
      return;
    }
    configLock(portMAX_DELAY);
    g_config.lock_policy = next;
    configUnlock();
    changed = true;
  }

  if (!doc["thresholds"].isNull()) {
    JsonArrayConst arr = doc["thresholds"].as<JsonArrayConst>();
    if (arr.isNull()) {
      sendError(400, "thresholds harus berupa array");
      return;
    }
    configLock(portMAX_DELAY);
    int i = 0;
    for (JsonVariantConst v : arr) {
      if (i >= SENSOR_COUNT) break;
      JsonObjectConst t = v.as<JsonObjectConst>();
      if (!t.isNull()) {
        float warnLow = t["warn_low"] | g_config.thresholds[i].warn_low;
        float alarmLow = t["alarm_low"] | g_config.thresholds[i].alarm_low;
        float warnHigh = t["warn_high"] | g_config.thresholds[i].warn_high;
        float alarmHigh = t["alarm_high"] | g_config.thresholds[i].alarm_high;
        float hysteresis = t["hysteresis"] | g_config.thresholds[i].hysteresis;
        if (alarmLow > warnLow || warnHigh > alarmHigh || warnLow >= warnHigh) {
          configUnlock();
          sendError(400, "Threshold tidak konsisten (alarm_low <= warn_low < warn_high <= alarm_high)");
          return;
        }
        if (hysteresis < 0) hysteresis = 0;
        g_config.thresholds[i].warn_low = warnLow;
        g_config.thresholds[i].alarm_low = alarmLow;
        g_config.thresholds[i].warn_high = warnHigh;
        g_config.thresholds[i].alarm_high = alarmHigh;
        g_config.thresholds[i].hysteresis = hysteresis;
        const char* unit = t["unit"] | "";
        if (unit[0]) {
          strncpy(g_config.thresholds[i].unit, unit, sizeof(g_config.thresholds[i].unit) - 1);
          g_config.thresholds[i].unit[sizeof(g_config.thresholds[i].unit) - 1] = 0;
        }
      }
      i++;
    }
    configUnlock();
    changed = true;
  }

  if (!doc["sensor_labels"].isNull()) {
    JsonArrayConst arr = doc["sensor_labels"].as<JsonArrayConst>();
    if (!arr.isNull()) {
      configLock(portMAX_DELAY);
      int i = 0;
      for (JsonVariantConst v : arr) {
        if (i >= SENSOR_COUNT) break;
        const char* label = v.as<const char*>();
        if (label && label[0]) {
          strncpy(g_config.labels[i], label, sizeof(g_config.labels[i]) - 1);
          g_config.labels[i][sizeof(g_config.labels[i]) - 1] = 0;
        }
        i++;
      }
      configUnlock();
      changed = true;
    }
  }

  if (!changed) {
    sendError(400, "Tidak ada perubahan");
    return;
  }

  configLock(portMAX_DELAY);
  storageSave();
  configUnlock();
  cloudApplyPolicy();
  sendOk("Konfigurasi disimpan");
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------
static void handleLogin() {
  if (!g_provisioned) {
    sendError(400, "Alat belum terdaftar");
    return;
  }
  if (s_loginLockedUntil != 0 && (int32_t)(millis() - s_loginLockedUntil) < 0) {
    sendError(429, "Terlalu banyak percobaan. Tunggu sebentar.");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    sendError(400, "JSON tidak valid");
    return;
  }
  String password = doc["password"] | "";
  char hash[65];
  passwordHash(password.c_str(), hash, sizeof(hash));

  if (strcmp(hash, g_config.admin_pass) != 0) {
    s_loginFailures++;
    if (s_loginFailures >= 5) {
      s_loginFailures = 0;
      s_loginLockedUntil = millis() + 60000UL;
      logAdd("warn", "login admin gagal 5x, dikunci 60 detik");
    }
    sendError(401, "Password salah");
    return;
  }

  s_loginFailures = 0;
  s_loginLockedUntil = 0;
  String token = createSession();
  char cookie[96];
  snprintf(
    cookie,
    sizeof(cookie),
    "mgsid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%lu",
    token.c_str(),
    (unsigned long)(SESSION_TTL_MS / 1000UL)
  );
  server.sendHeader("Set-Cookie", cookie);
  logAdd("info", "admin login dari %s", server.client().remoteIP().toString().c_str());
  sendOk("Login berhasil");
}

static void handleLogout() {
  destroySession();
  server.sendHeader("Set-Cookie", "mgsid=; Path=/; HttpOnly; Max-Age=0");
  sendOk("Logout");
}

// ---------------------------------------------------------------------------
// Alarm / cloud / logs
// ---------------------------------------------------------------------------
static void handleAlarmAck() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireUnlocked()) return;
  alarmAcknowledge();
  sendOk("Alarm di-acknowledge");
}

static void handleCloudSync() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!g_provisioned) {
    sendError(400, "Alat belum terdaftar");
    return;
  }
  if (!netIsConnected()) {
    sendError(503, "Wi-Fi tidak terhubung");
    return;
  }
  if (!cloudRequestSync()) {
    sendError(409, "Sync sedang berjalan");
    return;
  }
  sendOk("Sync cloud dimulai");
}

static void handleLogs() {
  if (!requireAuth()) return;

  int limit = 40;
  if (server.hasArg("limit")) {
    limit = server.arg("limit").toInt();
    if (limit < 1) limit = 1;
    if (limit > LOG_BUFFER_SIZE) limit = LOG_BUFFER_SIZE;
  }

  static LogEntry entries[LOG_BUFFER_SIZE];
  size_t count = logSnapshot(entries, (size_t)limit);

  JsonDocument doc;
  JsonArray arr = doc["logs"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["time"] = entries[i].time;
    o["level"] = entries[i].level;
    o["message"] = entries[i].message;
  }
  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
static void handleOtaRaw() {
  HTTPRaw& raw = server.raw();

  if (raw.status == RAW_START) {
    s_otaRawAccepted = false;
    if (!sessionValid() || server.header("X-CSRF-Token") != String(g_csrf) || g_ui_locked) {
      return;
    }
    s_otaRawAccepted = true;
    String sha = server.header("X-Firmware-SHA256");
    size_t total = (size_t)server.clientContentLength();
    if (!otaWebStart(total, sha)) {
      s_otaRawAccepted = false;
    }
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (s_otaRawAccepted) {
      otaWebWrite(raw.buf, raw.currentSize);
    }
    return;
  }

  if (raw.status == RAW_END) {
    return;
  }

  if (raw.status == RAW_ABORTED) {
    if (s_otaRawAccepted) otaWebAbort();
    s_otaRawAccepted = false;
  }
}

static void handleOtaFinal() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireUnlocked()) return;

  if (otaWebFinish()) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["message"] = "Firmware terpasang. Alat akan reboot.";
    doc["progress"] = 100;
    sendJson(200, doc);
  } else {
    sendError(400, g_ota_error[0] ? g_ota_error : "Upload firmware gagal");
  }
}

static void handleOtaStatus() {
  if (!requireAuth()) return;
  JsonDocument doc;
  doc["status"] = g_ota_status;
  doc["progress"] = g_ota_progress;
  doc["error"] = g_ota_error;
  doc["reboot_at"] = g_reboot_at;
  sendJson(200, doc);
}

static void handleOtaCloud() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireUnlocked()) return;

  configLock(portMAX_DELAY);
  bool available = strlen(g_config.fw_url) > 0;
  configUnlock();
  if (!available) {
    sendError(400, "Tidak ada firmware cloud untuk perangkat ini");
    return;
  }
  if (!otaCloudStart()) {
    sendError(409, g_ota_error[0] ? g_ota_error : "OTA tidak bisa dimulai");
    return;
  }
  sendOk("Download firmware dimulai");
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------
static void handleFactoryReset() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String confirm = doc["confirm"] | "";
  if (confirm != "RESET") {
    sendError(400, "Ketik RESET untuk konfirmasi");
    return;
  }

  logAdd("warn", "factory reset diminta oleh admin");
  s_factoryResetAt = millis() + 800UL;
  sendOk("Factory reset. Alat reboot ke mode registrasi.");
}

static void handleReboot() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  logAdd("warn", "reboot diminta oleh admin");
  scheduleReboot(800);
  sendOk("Rebooting");
}

// ---------------------------------------------------------------------------
// Provisioning / captive portal
// ---------------------------------------------------------------------------
static void handleScan() {
  if (!g_ap_mode) {
    sendError(400, "Scan hanya tersedia di mode registrasi");
    return;
  }

  int found = WiFi.scanNetworks();
  if (found < 0) {
    sendError(500, "Scan Wi-Fi gagal");
    return;
  }

  String seen[40];
  int seenCount = 0;
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();

  for (int i = 0; i < found && seenCount < 40; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool duplicate = false;
    for (int j = 0; j < seenCount; j++) {
      if (seen[j] == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    seen[seenCount++] = ssid;

    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(200, doc);
}

static void handleProvision() {
  if (!g_ap_mode) {
    sendError(400, "Alat tidak dalam mode registrasi");
    return;
  }
  if (!requireCsrf()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    sendError(400, "JSON tidak valid");
    return;
  }

  String email = doc["email"] | "";
  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  String adminPassword = doc["admin_password"] | "";
  String apiBase = doc["api_base"] | "";

  String error;
  String message;
  if (!provisioningProvision(email, ssid, password, adminPassword, apiBase, error, message)) {
    sendError(400, error.c_str());
    return;
  }
  sendOk(message.c_str());
}

// ---------------------------------------------------------------------------
// Not found / captive portal
// ---------------------------------------------------------------------------
static void handleNotFound() {
  if (g_ap_mode) {
    String location = String("http://") + WiFi.softAPIP().toString() + "/";
    redirectTo(location);
    return;
  }
  if (server.uri().startsWith("/api/")) {
    sendError(404, "not found");
    return;
  }
  redirectTo("/");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void webBegin() {
  randomHexToken(g_csrf, 16);
  for (int i = 0; i < MAX_SESSIONS; i++) {
    s_sessions[i].active = false;
    s_sessions[i].token[0] = 0;
    s_sessions[i].expiresAt = 0;
  }

  const char* headerKeys[] = {"Cookie", "X-CSRF-Token", "X-Firmware-SHA256"};
  server.collectHeaders(headerKeys, 3);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/setup", HTTP_GET, handleSetupPage);
  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/locked", HTTP_GET, handleLockedPage);
  server.on("/style.css", HTTP_GET, handleCss);
  server.on("/app.js", HTTP_GET, handleJs);
  server.on("/favicon.ico", HTTP_GET, handleFavicon);

  server.on("/api/system", HTTP_GET, handleSystem);
  server.on("/api/sensors", HTTP_GET, handleSensors);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/alarm/ack", HTTP_POST, handleAlarmAck);
  server.on("/api/cloud/sync", HTTP_POST, handleCloudSync);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/api/ota", HTTP_POST, handleOtaFinal, handleOtaRaw);
  server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
  server.on("/api/ota/cloud", HTTP_POST, handleOtaCloud);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/provision", HTTP_POST, handleProvision);

  server.onNotFound(handleNotFound);
  server.begin();
  logAdd("info", "web server aktif di port 80");
}

void webLoop() {
  server.handleClient();

  if (s_factoryResetAt != 0 && timeReached(s_factoryResetAt)) {
    s_factoryResetAt = 0;
    logAdd("warn", "menghapus konfigurasi dan reboot");
    storageClear();
    delay(150);
    ESP.restart();
  }
}
