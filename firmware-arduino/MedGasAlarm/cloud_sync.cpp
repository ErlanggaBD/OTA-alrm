#include "cloud_sync.h"
#include "app_config.h"
#include "app_state.h"
#include "log_buffer.h"
#include "net_manager.h"
#include "storage.h"

#include <HTTPClient.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

static TaskHandle_t s_cloudTask = nullptr;
static volatile bool s_cloudRunning = false;

void cloudSyncInternal();

static void cloudTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_cloudRunning = true;
    cloudSyncInternal();
    s_cloudRunning = false;
  }
}

static NetworkClient* makeClient(WiFiClient& plain, WiFiClientSecure& secure, const String& url) {
  if (url.startsWith("https://")) {
#if ALLOW_INSECURE_TLS
    secure.setInsecure();
#else
    if (strlen(API_ROOT_CA) > 0) secure.setCACert(API_ROOT_CA);
    else secure.setInsecure();
#endif
    return &secure;
  }
  return &plain;
}

static String sha256Hex(const String& data) {
  uint8_t hash[32];
  mbedtls_sha256((const unsigned char*)data.c_str(), data.length(), hash, 0);
  char out[65];
  for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
  out[64] = 0;
  return String(out);
}

static String hmacHex(const char* secret, const String& data) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t out[32];
  mbedtls_md_hmac(
    info,
    (const unsigned char*)secret,
    strlen(secret),
    (const unsigned char*)data.c_str(),
    data.length(),
    out
  );
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

static String randomNonce() {
  char out[33];
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  snprintf(out, sizeof(out), "%08lx%08lx", (unsigned long)a, (unsigned long)b);
  return String(out);
}

static int httpRequest(
  const String& method,
  const String& url,
  const String& body,
  const String& email,
  const String& timestamp,
  const String& nonce,
  const String& signature,
  String& response,
  String& responseSignature
) {
  WiFiClient plain;
  WiFiClientSecure secure;
  NetworkClient* client = makeClient(plain, secure, url);

  HTTPClient http;
  http.setTimeout(9000);
  http.setConnectTimeout(6000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* headerKeys[] = {"X-Signature"};
  http.collectHeaders(headerKeys, 1);

  if (!http.begin(*client, url)) {
    return -1;
  }

  if (email.length()) {
    http.addHeader("X-Device-Email", email);
    http.addHeader("X-Timestamp", timestamp);
    http.addHeader("X-Nonce", nonce);
    http.addHeader("X-Signature", signature);
  }
  if (method == "POST") {
    http.addHeader("Content-Type", "application/json");
  }

  int code = (method == "POST") ? http.POST(body) : http.GET();
  response = http.getString();
  responseSignature = http.header("X-Signature");
  http.end();
  return code;
}

void cloudBegin() {
  g_cloud.online = false;
  g_cloud.ever_synced = false;
  g_cloud.last_sync = 0;
  g_cloud.last_attempt = 0;
  g_cloud.last_error[0] = 0;

  if (s_cloudTask == nullptr) {
    xTaskCreate(cloudTaskFn, "cloud-sync", 12288, nullptr, 1, &s_cloudTask);
  }
}

bool cloudSyncBusy() {
  return s_cloudRunning;
}

bool cloudRequestSync() {
  if (s_cloudTask == nullptr) return false;
  if (s_cloudRunning) return false;
  xTaskNotifyGive(s_cloudTask);
  return true;
}

bool cloudProvision(const String& apiBase, const String& email, const String& serial, String& responseOut) {
  JsonDocument bodyDoc;
  bodyDoc["email"] = email;
  bodyDoc["serial"] = serial;
  bodyDoc["hw_rev"] = HW_REV;
  bodyDoc["fw_version"] = FW_VERSION;
  String body;
  serializeJson(bodyDoc, body);

  String url = apiBase + "/api/v1/provision";
  String sig;
  int code = httpRequest("POST", url, body, "", "", "", "", responseOut, sig);
  if (code == 200) {
    logAdd("info", "provisioning cloud sukses");
    return true;
  }

  snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "provision HTTP %d", code);
  logAdd("error", "provisioning cloud gagal (HTTP %d): %s", code, responseOut.c_str());
  return false;
}

static void applyQuota(JsonObjectConst quota) {
  if (quota.isNull()) return;
  const char* plan = quota["plan"] | "basic";
  strncpy(g_config.plan, plan, sizeof(g_config.plan) - 1);
  g_config.plan[sizeof(g_config.plan) - 1] = 0;

  uint32_t expires = quota["expires_epoch"] | 0;
  g_config.quota_expires = expires;
}

bool cloudApplyConfig(JsonObjectConst config, bool save) {
  if (config.isNull()) return false;
  configLock(portMAX_DELAY);

  int active = config["active_sensors"] | (int)g_config.active_sensors;
  if (active < 1) active = 1;
  if (active > SENSOR_COUNT) active = SENSOR_COUNT;
  bool changedActive = (uint8_t)active != g_config.active_sensors;
  g_config.active_sensors = (uint8_t)active;

  JsonArrayConst labels = config["sensor_labels"].as<JsonArrayConst>();
  if (!labels.isNull()) {
    int i = 0;
    for (JsonVariantConst v : labels) {
      if (i >= SENSOR_COUNT) break;
      const char* label = v.as<const char*>();
      if (label && label[0]) {
        strncpy(g_config.labels[i], label, sizeof(g_config.labels[i]) - 1);
        g_config.labels[i][sizeof(g_config.labels[i]) - 1] = 0;
      }
      i++;
    }
  }

  JsonArrayConst thresholds = config["thresholds"].as<JsonArrayConst>();
  if (!thresholds.isNull()) {
    int i = 0;
    for (JsonVariantConst v : thresholds) {
      if (i >= SENSOR_COUNT) break;
      JsonObjectConst t = v.as<JsonObjectConst>();
      if (!t.isNull()) {
        g_config.thresholds[i].warn_low = t["warn_low"] | g_config.thresholds[i].warn_low;
        g_config.thresholds[i].alarm_low = t["alarm_low"] | g_config.thresholds[i].alarm_low;
        g_config.thresholds[i].warn_high = t["warn_high"] | g_config.thresholds[i].warn_high;
        g_config.thresholds[i].alarm_high = t["alarm_high"] | g_config.thresholds[i].alarm_high;
        g_config.thresholds[i].hysteresis = t["hysteresis"] | g_config.thresholds[i].hysteresis;
        const char* unit = t["unit"] | "";
        if (unit[0]) {
          strncpy(g_config.thresholds[i].unit, unit, sizeof(g_config.thresholds[i].unit) - 1);
          g_config.thresholds[i].unit[sizeof(g_config.thresholds[i].unit) - 1] = 0;
        }
      }
      i++;
    }
  }

  const char* policy = config["lock_policy"] | "";
  if (strcmp(policy, "grace") == 0) g_config.lock_policy = LOCK_GRACE;
  else if (strcmp(policy, "strict") == 0) g_config.lock_policy = LOCK_STRICT;
  else if (strcmp(policy, "provision_only") == 0) g_config.lock_policy = LOCK_PROVISION_ONLY;

  if (config["allow_local_override"].is<bool>()) {
    g_config.allow_local_override = config["allow_local_override"].as<bool>();
  }
  if (config["auto_ota"].is<bool>()) {
    g_config.auto_ota = config["auto_ota"].as<bool>();
  }
  int grace = config["grace_days"] | (int)g_config.grace_days;
  if (grace < 0) grace = 0;
  if (grace > 365) grace = 365;
  g_config.grace_days = (uint16_t)grace;

  const char* offline = config["offline_action"] | "";
  if (strcmp(offline, "fallback_default") == 0) g_config.offline_action = OFFLINE_FALLBACK_DEFAULT;
  else if (strcmp(offline, "lock_ui") == 0) g_config.offline_action = OFFLINE_LOCK_UI;
  else if (strcmp(offline, "keep_last") == 0) g_config.offline_action = OFFLINE_KEEP_LAST;

  int defaultActive = config["default_active_sensors"] | (int)g_config.default_active_sensors;
  if (defaultActive < 1) defaultActive = 1;
  if (defaultActive > SENSOR_COUNT) defaultActive = SENSOR_COUNT;
  g_config.default_active_sensors = (uint8_t)defaultActive;

  applyQuota(config["quota"].as<JsonObjectConst>());

  JsonObjectConst fw = config["firmware"].as<JsonObjectConst>();
  g_config.fw_url[0] = 0;
  g_config.fw_sha[0] = 0;
  g_config.fw_available_version[0] = 0;
  g_config.fw_size = 0;
  if (!fw.isNull()) {
    const char* version = fw["version"] | "";
    const char* url = fw["url"] | "";
    const char* sha = fw["sha256"] | "";
    if (version[0] && strcmp(version, FW_VERSION) != 0) {
      strncpy(g_config.fw_available_version, version, sizeof(g_config.fw_available_version) - 1);
      g_config.fw_available_version[sizeof(g_config.fw_available_version) - 1] = 0;
      strncpy(g_config.fw_url, url, sizeof(g_config.fw_url) - 1);
      g_config.fw_url[sizeof(g_config.fw_url) - 1] = 0;
      strncpy(g_config.fw_sha, sha, sizeof(g_config.fw_sha) - 1);
      g_config.fw_sha[sizeof(g_config.fw_sha) - 1] = 0;
      g_config.fw_size = fw["size"] | 0;
    }
  }

  if (changedActive) {
    logAdd("info", "kuota sensor diupdate menjadi %u", (unsigned)g_config.active_sensors);
  }
  if (save) storageSave();
  configUnlock();
  return true;
}

static void setLocked(bool locked, const char* reason) {
  bool changed = g_ui_locked != locked;
  g_ui_locked = locked;
  if (locked) {
    strncpy(g_lock_reason, reason ? reason : "Dashboard terkunci", sizeof(g_lock_reason) - 1);
    g_lock_reason[sizeof(g_lock_reason) - 1] = 0;
  } else {
    g_lock_reason[0] = 0;
  }
  if (changed) {
    logAdd(locked ? "warn" : "info", "UI %s: %s", locked ? "dikunci" : "dibuka", g_lock_reason);
  }
}

void cloudApplyPolicy() {
  uint32_t now = nowEpoch();
  if (now == 0) {
    // Without time we cannot evaluate quota expiry. Do not punish the user.
    if (g_config.lock_policy == LOCK_STRICT && !g_cloud.online) {
      setLocked(true, "Mode strict: internet/NTP tidak tersedia");
    } else {
      setLocked(false, "");
    }
    return;
  }

  if (g_config.lock_policy == LOCK_STRICT && !g_cloud.online) {
    setLocked(true, "Mode strict: alat membutuhkan koneksi internet");
    return;
  }

  bool quotaExpired = g_config.quota_expires > 0 && now > g_config.quota_expires;
  bool syncStale = g_config.lock_policy == LOCK_GRACE && g_cloud.last_sync > 0 &&
                   now > g_cloud.last_sync + (uint32_t)g_config.grace_days * 86400UL;

  if (!quotaExpired && !syncStale) {
    setLocked(false, "");
    return;
  }

  uint32_t deadline = g_config.quota_expires;
  if (syncStale && g_cloud.last_sync > deadline) {
    deadline = g_cloud.last_sync;
  }
  if (deadline > 0 && now <= deadline + (uint32_t)g_config.grace_days * 86400UL) {
    setLocked(false, "");
    return;
  }

  switch (g_config.offline_action) {
    case OFFLINE_FALLBACK_DEFAULT:
      configLock(portMAX_DELAY);
      g_config.active_sensors = g_config.default_active_sensors;
      storageSave();
      configUnlock();
      setLocked(false, "");
      logAdd("warn", "offline: fallback ke %u sensor", (unsigned)g_config.active_sensors);
      break;
    case OFFLINE_LOCK_UI:
      setLocked(true, "Kuota kedaluwarsa / offline melewati grace period");
      break;
    case OFFLINE_KEEP_LAST:
    default:
      setLocked(false, "");
      break;
  }
}

void cloudSyncInternal() {
  if (!g_provisioned) return;
  if (!netIsConnected()) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "Wi-Fi tidak terhubung");
    cloudApplyPolicy();
    return;
  }
  if (!timeIsValid()) netSyncTime();
  if (!timeIsValid()) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "waktu NTP belum tersedia");
    logAdd("warn", "sync ditunda: NTP belum tersedia");
    cloudApplyPolicy();
    return;
  }

  char email[sizeof(g_config.email)];
  char secret[sizeof(g_config.device_secret)];
  char apiBase[sizeof(g_config.api_base)];
  configLock(portMAX_DELAY);
  strncpy(email, g_config.email, sizeof(email) - 1);
  email[sizeof(email) - 1] = 0;
  strncpy(secret, g_config.device_secret, sizeof(secret) - 1);
  secret[sizeof(secret) - 1] = 0;
  strncpy(apiBase, g_config.api_base, sizeof(apiBase) - 1);
  apiBase[sizeof(apiBase) - 1] = 0;
  configUnlock();

  String path = String("/api/v1/devices/") + email + "/config";
  String url = String(apiBase) + path;
  String ts = String(nowEpoch());
  String nonce = randomNonce();
  String canonical = String("GET\n") + path + "\n" + ts + "\n" + sha256Hex("");
  String signature = hmacHex(secret, canonical);

  String response;
  String responseSig;
  int code = httpRequest("GET", url, "", email, ts, nonce, signature, response, responseSig);
  g_cloud.last_attempt = millis();

  if (code != 200) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "config HTTP %d", code);
    logAdd("error", "sync config gagal (HTTP %d)", code);
    cloudApplyPolicy();
    return;
  }

  String expected = hmacHex(secret, response);
  if (responseSig.length() == 0 || expected != responseSig) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "signature respons tidak valid");
    logAdd("error", "sync ditolak: signature respons tidak valid");
    cloudApplyPolicy();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "JSON config invalid");
    logAdd("error", "JSON config invalid: %s", err.c_str());
    cloudApplyPolicy();
    return;
  }

  cloudApplyConfig(doc.as<JsonObjectConst>(), true);
  g_cloud.online = true;
  g_cloud.ever_synced = true;
  g_cloud.last_sync = nowEpoch();
  configLock(portMAX_DELAY);
  g_config.last_sync = g_cloud.last_sync;
  storageSave();
  configUnlock();
  g_cloud.last_error[0] = 0;
  cloudApplyPolicy();
  logAdd("info", "sync cloud sukses, %u sensor aktif", (unsigned)g_config.active_sensors);

  // Heartbeat is best-effort; failure must not invalidate the config.
  JsonDocument hb;
  hb["fw_version"] = FW_VERSION;
  hb["active_sensors"] = g_config.active_sensors;
  hb["wifi_rssi"] = netRssi();
  hb["uptime"] = millis() / 1000;
  hb["alarm_state"] = alarmLevelName(g_alarm.state);
  hb["online"] = true;
  String hbBody;
  serializeJson(hb, hbBody);

  String hbPath = String("/api/v1/devices/") + email + "/heartbeat";
  String hbUrl = String(apiBase) + hbPath;
  String hbTs = String(nowEpoch());
  String hbNonce = randomNonce();
  String hbCanonical = String("POST\n") + hbPath + "\n" + hbTs + "\n" + sha256Hex(hbBody);
  String hbSig = hmacHex(secret, hbCanonical);
  String hbResponse;
  String hbResponseSig;
  httpRequest("POST", hbUrl, hbBody, email, hbTs, hbNonce, hbSig, hbResponse, hbResponseSig);
}

void cloudLoop() {
  if (!g_provisioned || g_ap_mode) return;
  if (s_cloudRunning) return;
  if (g_cloud.last_attempt != 0 &&
      millis() - g_cloud.last_attempt < CLOUD_RETRY_INTERVAL_MS) {
    return;
  }

  uint32_t now = nowEpoch();
  if (g_cloud.last_sync != 0 && now != 0 &&
      now - g_cloud.last_sync < (CLOUD_SYNC_INTERVAL_MS / 1000UL)) {
    return;
  }
  cloudRequestSync();
}
