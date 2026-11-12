#include "web_server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "alarm.h"
#include "app_config.h"
#include "app_state.h"
#include "cJSON.h"
#include "cloud_sync.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "log_buffer.h"
#include "net_manager.h"
#include "ota_manager.h"
#include "provisioning.h"
#include "sensors.h"
#include "storage.h"

static const char *TAG = "web";

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
typedef struct {
  char token[33];
  int64_t expires_ms;
  bool active;
} session_t;

static httpd_handle_t s_server = NULL;
static char s_csrf[33];
static session_t s_sessions[MAX_SESSIONS];
static uint8_t s_login_failures = 0;
static uint32_t s_login_locked_until = 0;
static uint32_t s_factory_reset_at = 0;

static int64_t now_ms(void) {
  return esp_timer_get_time() / 1000LL;
}

static void random_hex(char *out, size_t bytes) {
  static const char *hex = "0123456789abcdef";
  for (size_t i = 0; i < bytes; i++) {
    uint8_t v = (uint8_t)(esp_random() & 0xFF);
    out[i * 2] = hex[(v >> 4) & 0x0F];
    out[i * 2 + 1] = hex[v & 0x0F];
  }
  out[bytes * 2] = 0;
}

static void get_cookie(httpd_req_t *req, const char *name, char *out, size_t len) {
  out[0] = 0;
  char header[512];
  if (httpd_req_get_hdr_value_str(req, "Cookie", header, sizeof(header)) != ESP_OK) return;

  char needle[64];
  snprintf(needle, sizeof(needle), "%s=", name);
  char *p = strstr(header, needle);
  if (!p) return;
  p += strlen(needle);
  char *end = strchr(p, ';');
  size_t n = end ? (size_t)(end - p) : strlen(p);
  if (n >= len) n = len - 1;
  memcpy(out, p, n);
  out[n] = 0;
}

static int find_session(const char *token) {
  if (!token || !token[0]) return -1;
  int64_t now = now_ms();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!s_sessions[i].active) continue;
    if (strcmp(token, s_sessions[i].token) != 0) continue;
    if (now >= s_sessions[i].expires_ms) {
      s_sessions[i].active = false;
      return -1;
    }
    return i;
  }
  return -1;
}

static bool session_valid(httpd_req_t *req) {
  char token[33];
  get_cookie(req, "mgsid", token, sizeof(token));
  int idx = find_session(token);
  if (idx < 0) return false;
  s_sessions[idx].expires_ms = now_ms() + SESSION_TTL_MS;
  return true;
}

static void create_session(char *out, size_t len) {
  int slot = -1;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!s_sessions[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < MAX_SESSIONS; i++) {
      if (s_sessions[i].expires_ms < s_sessions[slot].expires_ms) slot = i;
    }
  }
  random_hex(s_sessions[slot].token, 16);
  s_sessions[slot].expires_ms = now_ms() + SESSION_TTL_MS;
  s_sessions[slot].active = true;
  snprintf(out, len, "%s", s_sessions[slot].token);
}

static void destroy_session(httpd_req_t *req) {
  char token[33];
  get_cookie(req, "mgsid", token, sizeof(token));
  int idx = find_session(token);
  if (idx >= 0) {
    s_sessions[idx].active = false;
    s_sessions[idx].token[0] = 0;
  }
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
static const char *status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 423: return "Locked";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
  free(out);
  return err;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "error", message ? message : "error");
  char status[40];
  snprintf(status, sizeof(status), "%d %s", code, status_text(code));
  httpd_resp_set_status(req, status);
  return send_json(req, root);
}

static esp_err_t send_ok(httpd_req_t *req, const char *message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  if (message) cJSON_AddStringToObject(root, "message", message);
  return send_json(req, root);
}

static bool require_auth(httpd_req_t *req) {
  if (!session_valid(req)) {
    send_error(req, 401, "unauthorized");
    return false;
  }
  return true;
}

static bool require_csrf(httpd_req_t *req) {
  char token[64];
  if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", token, sizeof(token)) != ESP_OK ||
      strcmp(token, s_csrf) != 0) {
    send_error(req, 403, "CSRF token invalid");
    return false;
  }
  return true;
}

static bool require_unlocked(httpd_req_t *req) {
  if (g_ui_locked) {
    send_error(req, 423, g_lock_reason[0] ? g_lock_reason : "locked");
    return false;
  }
  return true;
}

static char *read_body(httpd_req_t *req, size_t max_len) {
  if (req->content_len == 0 || req->content_len > max_len) return NULL;
  char *buf = malloc(req->content_len + 1);
  if (!buf) return NULL;
  size_t received = 0;
  while (received < req->content_len) {
    int r = httpd_req_recv(req, buf + received, req->content_len - received);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) {
      free(buf);
      return NULL;
    }
    received += (size_t)r;
  }
  buf[received] = 0;
  return buf;
}

static bool query_param(httpd_req_t *req, const char *key, char *out, size_t len) {
  char query[160];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
  char value[64];
  if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return false;
  snprintf(out, len, "%s", value);
  return true;
}

static void iso_from_epoch(uint32_t epoch, char *out, size_t len) {
  if (!epoch) {
    if (len) out[0] = 0;
    return;
  }
  time_t t = (time_t)epoch;
  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static float get_float(cJSON *obj, const char *key, float def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

static int get_int(cJSON *obj, const char *key, int def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? item->valueint : def;
}

static const char *get_str(cJSON *obj, const char *key, const char *def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : def;
}

// ---------------------------------------------------------------------------
// Static assets (SPIFFS)
// ---------------------------------------------------------------------------
static esp_err_t serve_file(httpd_req_t *req, const char *file, const char *type, bool cache) {
  char path[64];
  snprintf(path, sizeof(path), "/spiffs/%s", file);
  FILE *f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "asset hilang: %s", path);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "asset missing");
  }

  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", cache ? "public, max-age=3600" : "no-store");

  char buf[1024];
  size_t read;
  while ((read = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, read) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req) {
  if (!g_provisioned || g_ap_mode) {
    return serve_file(req, "setup.html", "text/html", false);
  }
  return serve_file(req, "index.html", "text/html", false);
}

static esp_err_t setup_handler(httpd_req_t *req) {
  return serve_file(req, "setup.html", "text/html", false);
}

static esp_err_t settings_handler(httpd_req_t *req) {
  if (!g_provisioned) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
  }
  return serve_file(req, "settings.html", "text/html", false);
}

static esp_err_t login_page_handler(httpd_req_t *req) {
  return serve_file(req, "login.html", "text/html", false);
}

static esp_err_t locked_page_handler(httpd_req_t *req) {
  return serve_file(req, "locked.html", "text/html", false);
}

static esp_err_t css_handler(httpd_req_t *req) {
  return serve_file(req, "style.css", "text/css", true);
}

static esp_err_t js_handler(httpd_req_t *req) {
  return serve_file(req, "app.js", "application/javascript", true);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// GET /api/system
// ---------------------------------------------------------------------------
static esp_err_t system_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "provisioned", g_provisioned);
  cJSON_AddBoolToObject(root, "ap_mode", g_ap_mode);
  cJSON_AddBoolToObject(root, "locked", g_ui_locked);
  cJSON_AddStringToObject(root, "lock_reason", g_lock_reason);
  cJSON_AddStringToObject(root, "csrf", s_csrf);
  cJSON_AddStringToObject(root, "ap_ssid", g_ap_ssid);
  cJSON_AddStringToObject(root, "ap_pass", g_ap_pass);

  cJSON *dev = cJSON_AddObjectToObject(root, "device");
  cJSON_AddStringToObject(dev, "serial", g_serial);
  cJSON_AddStringToObject(dev, "hw", HW_REV);
  cJSON_AddStringToObject(dev, "fw", FW_VERSION);
  cJSON_AddNumberToObject(dev, "uptime", (double)(millis_now() / 1000UL));
  cJSON_AddStringToObject(dev, "ip", net_ip());
  cJSON_AddNumberToObject(dev, "rssi", net_rssi());
  cJSON_AddStringToObject(dev, "ssid", net_ssid());

  cJSON *cloud = cJSON_AddObjectToObject(root, "cloud");
  cJSON_AddBoolToObject(cloud, "online", g_cloud.online);
  cJSON_AddNumberToObject(cloud, "last_sync", (double)g_cloud.last_sync);
  cJSON_AddStringToObject(cloud, "last_error", g_cloud.last_error);

  cJSON *alarm = cJSON_AddObjectToObject(root, "alarm");
  cJSON_AddStringToObject(alarm, "state", alarm_level_name(g_alarm.state));
  cJSON_AddBoolToObject(alarm, "latched", g_alarm.latched);
  cJSON_AddBoolToObject(alarm, "acknowledged", g_alarm.acknowledged);
  cJSON_AddStringToObject(alarm, "summary", g_alarm.summary);

  cJSON *sensors = cJSON_AddObjectToObject(root, "sensors");
  cJSON *ota = cJSON_AddObjectToObject(root, "ota");
  cJSON *triggered = cJSON_AddArrayToObject(alarm, "triggered");

  config_lock(0);
  cJSON_AddStringToObject(dev, "email", g_config.email);
  cJSON_AddStringToObject(cloud, "plan", g_config.plan);
  cJSON_AddNumberToObject(cloud, "quota_expires", (double)g_config.quota_expires);
  cJSON_AddStringToObject(cloud, "policy", lock_policy_name(g_config.lock_policy));
  cJSON_AddNumberToObject(cloud, "grace_days", g_config.grace_days);
  cJSON_AddStringToObject(cloud, "offline_action", offline_action_name(g_config.offline_action));
  cJSON_AddNumberToObject(sensors, "active", g_config.active_sensors);
  cJSON_AddNumberToObject(sensors, "total", SENSOR_COUNT);
  cJSON_AddBoolToObject(ota, "available", strlen(g_config.fw_url) > 0);
  cJSON_AddStringToObject(ota, "version", g_config.fw_available_version);
  cJSON_AddNumberToObject(ota, "size", (double)g_config.fw_size);

  for (uint8_t i = 0; i < g_config.active_sensors; i++) {
    if (!(g_alarm.active_mask & (1UL << i))) continue;
    cJSON *t = cJSON_CreateObject();
    cJSON_AddNumberToObject(t, "idx", i);
    cJSON_AddStringToObject(t, "label", g_config.labels[i]);
    cJSON_AddItemToArray(triggered, t);
  }
  config_unlock();

  cJSON_AddStringToObject(ota, "status", g_ota_status);
  cJSON_AddNumberToObject(ota, "progress", g_ota_progress);
  cJSON_AddStringToObject(ota, "error", g_ota_error);

  return send_json(req, root);
}

// ---------------------------------------------------------------------------
// GET /api/sensors
// ---------------------------------------------------------------------------
static esp_err_t sensors_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_unlocked(req)) return ESP_OK;

  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "sensors");

  config_lock(0);
  uint8_t active = g_config.active_sensors;
  for (uint8_t i = 0; i < active && i < SENSOR_COUNT; i++) {
    const SensorState *s = &g_sensors[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToArray(arr, o);
    cJSON_AddNumberToObject(o, "idx", i);
    cJSON_AddStringToObject(o, "label", g_config.labels[i]);
    cJSON_AddStringToObject(o, "unit", g_config.thresholds[i].unit);
    cJSON_AddStringToObject(o, "secondary_unit", "%");
    cJSON_AddBoolToObject(o, "valid", s->valid);
    cJSON_AddBoolToObject(o, "fault", s->state == ALARM_FAULT);
    cJSON_AddStringToObject(o, "state", alarm_level_name(s->state));
    cJSON_AddNumberToObject(o, "age", sensor_age_seconds(i));
    cJSON_AddNumberToObject(o, "last_read", (double)s->last_read_epoch);
    if (s->valid && !isnan(s->primary)) cJSON_AddNumberToObject(o, "value", s->primary);
    else cJSON_AddNullToObject(o, "value");
    if (s->valid && !isnan(s->secondary)) cJSON_AddNumberToObject(o, "secondary", s->secondary);
    else cJSON_AddNullToObject(o, "secondary");
  }
  config_unlock();

  return send_json(req, root);
}

// ---------------------------------------------------------------------------
// /api/config
// ---------------------------------------------------------------------------
static esp_err_t config_get_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_unlocked(req)) return ESP_OK;

  cJSON *root = cJSON_CreateObject();
  config_lock(0);
  cJSON_AddNumberToObject(root, "active_sensors", g_config.active_sensors);
  cJSON_AddNumberToObject(root, "default_active_sensors", g_config.default_active_sensors);
  cJSON_AddStringToObject(root, "lock_policy", lock_policy_name(g_config.lock_policy));
  cJSON_AddBoolToObject(root, "allow_local_override", g_config.allow_local_override);
  cJSON_AddNumberToObject(root, "grace_days", g_config.grace_days);
  cJSON_AddStringToObject(root, "offline_action", offline_action_name(g_config.offline_action));

  cJSON *labels = cJSON_AddArrayToObject(root, "sensor_labels");
  cJSON *thresholds = cJSON_AddArrayToObject(root, "thresholds");
  for (int i = 0; i < SENSOR_COUNT; i++) {
    cJSON_AddItemToArray(labels, cJSON_CreateString(g_config.labels[i]));
    cJSON *t = cJSON_CreateObject();
    cJSON_AddNumberToObject(t, "idx", i);
    cJSON_AddNumberToObject(t, "warn_low", g_config.thresholds[i].warn_low);
    cJSON_AddNumberToObject(t, "alarm_low", g_config.thresholds[i].alarm_low);
    cJSON_AddNumberToObject(t, "warn_high", g_config.thresholds[i].warn_high);
    cJSON_AddNumberToObject(t, "alarm_high", g_config.thresholds[i].alarm_high);
    cJSON_AddNumberToObject(t, "hysteresis", g_config.thresholds[i].hysteresis);
    cJSON_AddStringToObject(t, "unit", g_config.thresholds[i].unit);
    cJSON_AddItemToArray(thresholds, t);
  }

  cJSON *quota = cJSON_AddObjectToObject(root, "quota");
  cJSON_AddStringToObject(quota, "plan", g_config.plan);
  cJSON_AddNumberToObject(quota, "expires_epoch", (double)g_config.quota_expires);
  char iso[24];
  iso_from_epoch(g_config.quota_expires, iso, sizeof(iso));
  cJSON_AddStringToObject(quota, "expires_at", iso);

  cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
  cJSON_AddStringToObject(fw, "version", g_config.fw_available_version);
  cJSON_AddStringToObject(fw, "url", g_config.fw_url);
  cJSON_AddStringToObject(fw, "sha256", g_config.fw_sha);
  cJSON_AddNumberToObject(fw, "size", (double)g_config.fw_size);
  config_unlock();

  cJSON_AddNumberToObject(root, "server_time", (double)now_epoch());
  cJSON_AddNumberToObject(root, "issued_at", (double)now_epoch());
  cJSON_AddNumberToObject(root, "expires_at", (double)(now_epoch() + 86400UL));
  return send_json(req, root);
}

static esp_err_t config_post_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;
  if (!require_unlocked(req)) return ESP_OK;

  char *body = read_body(req, 16384);
  if (!body) return send_error(req, 400, "Body JSON tidak valid");

  cJSON *doc = cJSON_Parse(body);
  free(body);
  if (!doc) return send_error(req, 400, "JSON tidak valid");

  bool changed = false;

  cJSON *policy = cJSON_GetObjectItemCaseSensitive(doc, "lock_policy");
  if (policy) {
    if (!g_config.allow_local_override) {
      cJSON_Delete(doc);
      return send_error(req, 403, "Perubahan lock policy dikunci oleh vendor");
    }
    const char *name = cJSON_IsString(policy) ? policy->valuestring : "";
    uint8_t next;
    if (strcmp(name, "provision_only") == 0) next = LOCK_PROVISION_ONLY;
    else if (strcmp(name, "grace") == 0) next = LOCK_GRACE;
    else if (strcmp(name, "strict") == 0) next = LOCK_STRICT;
    else {
      cJSON_Delete(doc);
      return send_error(req, 400, "lock_policy tidak valid");
    }
    config_lock(0);
    g_config.lock_policy = next;
    config_unlock();
    changed = true;
  }

  cJSON *thresholds = cJSON_GetObjectItemCaseSensitive(doc, "thresholds");
  if (thresholds) {
    if (!cJSON_IsArray(thresholds)) {
      cJSON_Delete(doc);
      return send_error(req, 400, "thresholds harus berupa array");
    }
    config_lock(0);
    int i = 0;
    cJSON *v;
    cJSON_ArrayForEach(v, thresholds) {
      if (i >= SENSOR_COUNT) break;
      if (cJSON_IsObject(v)) {
        Threshold *t = &g_config.thresholds[i];
        float warn_low = get_float(v, "warn_low", t->warn_low);
        float alarm_low = get_float(v, "alarm_low", t->alarm_low);
        float warn_high = get_float(v, "warn_high", t->warn_high);
        float alarm_high = get_float(v, "alarm_high", t->alarm_high);
        float hysteresis = get_float(v, "hysteresis", t->hysteresis);
        if (alarm_low > warn_low || warn_high > alarm_high || warn_low >= warn_high) {
          config_unlock();
          cJSON_Delete(doc);
          return send_error(
            req, 400,
            "Threshold tidak konsisten (alarm_low <= warn_low < warn_high <= alarm_high)");
        }
        if (hysteresis < 0) hysteresis = 0;
        t->warn_low = warn_low;
        t->alarm_low = alarm_low;
        t->warn_high = warn_high;
        t->alarm_high = alarm_high;
        t->hysteresis = hysteresis;
        const char *unit = get_str(v, "unit", NULL);
        if (unit && unit[0]) snprintf(t->unit, sizeof(t->unit), "%s", unit);
      }
      i++;
    }
    config_unlock();
    changed = true;
  }

  cJSON *labels = cJSON_GetObjectItemCaseSensitive(doc, "sensor_labels");
  if (labels) {
    if (!cJSON_IsArray(labels)) {
      cJSON_Delete(doc);
      return send_error(req, 400, "sensor_labels harus berupa array");
    }
    config_lock(0);
    int i = 0;
    cJSON *v;
    cJSON_ArrayForEach(v, labels) {
      if (i >= SENSOR_COUNT) break;
      if (cJSON_IsString(v) && v->valuestring[0]) {
        snprintf(g_config.labels[i], sizeof(g_config.labels[i]), "%s", v->valuestring);
      }
      i++;
    }
    config_unlock();
    changed = true;
  }

  cJSON_Delete(doc);

  if (!changed) return send_error(req, 400, "Tidak ada perubahan");

  config_lock(0);
  storage_save();
  config_unlock();
  cloud_apply_policy();
  return send_ok(req, "Konfigurasi disimpan");
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------
static esp_err_t login_handler(httpd_req_t *req) {
  if (!g_provisioned) return send_error(req, 400, "Alat belum terdaftar");
  if (s_login_locked_until != 0 && (int32_t)(millis_now() - s_login_locked_until) < 0) {
    return send_error(req, 429, "Terlalu banyak percobaan. Tunggu sebentar.");
  }

  char *body = read_body(req, 4096);
  if (!body) return send_error(req, 400, "Body JSON tidak valid");
  cJSON *doc = cJSON_Parse(body);
  free(body);
  if (!doc) return send_error(req, 400, "JSON tidak valid");

  const char *password = get_str(doc, "password", "");
  char hash[65];
  password_hash(password, hash, sizeof(hash));

  if (strcmp(hash, g_config.admin_pass) != 0) {
    s_login_failures++;
    if (s_login_failures >= 5) {
      s_login_failures = 0;
      s_login_locked_until = millis_now() + 60000UL;
      log_add("warn", "login admin gagal 5x, dikunci 60 detik");
    }
    cJSON_Delete(doc);
    return send_error(req, 401, "Password salah");
  }

  s_login_failures = 0;
  s_login_locked_until = 0;
  cJSON_Delete(doc);

  char token[33];
  create_session(token, sizeof(token));
  char cookie[96];
  snprintf(cookie, sizeof(cookie),
           "mgsid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%lu", token,
           (unsigned long)(SESSION_TTL_MS / 1000UL));
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
  log_add("info", "admin login berhasil");
  return send_ok(req, "Login berhasil");
}

static esp_err_t logout_handler(httpd_req_t *req) {
  destroy_session(req);
  httpd_resp_set_hdr(req, "Set-Cookie", "mgsid=; Path=/; HttpOnly; Max-Age=0");
  return send_ok(req, "Logout");
}

// ---------------------------------------------------------------------------
// Alarm / cloud / logs
// ---------------------------------------------------------------------------
static esp_err_t alarm_ack_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;
  if (!require_unlocked(req)) return ESP_OK;
  alarm_acknowledge();
  return send_ok(req, "Alarm di-acknowledge");
}

static esp_err_t cloud_sync_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;
  if (!g_provisioned) return send_error(req, 400, "Alat belum terdaftar");
  if (!net_is_connected()) return send_error(req, 503, "Wi-Fi tidak terhubung");
  if (!cloud_request_sync()) return send_error(req, 409, "Sync sedang berjalan");
  return send_ok(req, "Sync cloud dimulai");
}

static esp_err_t logs_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;

  int limit = 40;
  char value[16];
  if (query_param(req, "limit", value, sizeof(value))) {
    limit = atoi(value);
    if (limit < 1) limit = 1;
    if (limit > LOG_BUFFER_SIZE) limit = LOG_BUFFER_SIZE;
  }

  LogEntry *entries = calloc((size_t)limit, sizeof(LogEntry));
  if (!entries) return send_error(req, 500, "out of memory");
  size_t count = log_snapshot(entries, (size_t)limit);

  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "logs");
  for (size_t i = 0; i < count; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "time", entries[i].time);
    cJSON_AddStringToObject(o, "level", entries[i].level);
    cJSON_AddStringToObject(o, "message", entries[i].message);
    cJSON_AddItemToArray(arr, o);
  }
  free(entries);
  return send_json(req, root);
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
static esp_err_t ota_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;
  if (!require_unlocked(req)) return ESP_OK;
  if (req->content_len == 0) return send_error(req, 400, "Ukuran firmware tidak valid");

  char sha[80] = {0};
  httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", sha, sizeof(sha));

  if (!ota_web_begin(req->content_len, sha)) {
    return send_error(req, 400, g_ota_error[0] ? g_ota_error : "Upload firmware gagal");
  }

  size_t remaining = req->content_len;
  uint8_t buf[2048];
  while (remaining > 0) {
    int want = remaining > sizeof(buf) ? (int)sizeof(buf) : (int)remaining;
    int r = httpd_req_recv(req, (char *)buf, want);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) {
      ota_web_abort();
      return send_error(req, 400, "Upload terputus");
    }
    if (!ota_web_write(buf, (size_t)r)) {
      return send_error(req, 400, g_ota_error[0] ? g_ota_error : "Upload firmware gagal");
    }
    remaining -= (size_t)r;
  }

  if (!ota_web_finish()) {
    return send_error(req, 400, g_ota_error[0] ? g_ota_error : "Upload firmware gagal");
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "message", "Firmware terpasang. Alat akan reboot.");
  cJSON_AddNumberToObject(root, "progress", 100);
  return send_json(req, root);
}

static esp_err_t ota_status_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", g_ota_status);
  cJSON_AddNumberToObject(root, "progress", g_ota_progress);
  cJSON_AddStringToObject(root, "error", g_ota_error);
  cJSON_AddNumberToObject(root, "reboot_at", (double)g_reboot_at);
  return send_json(req, root);
}

static esp_err_t ota_cloud_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;
  if (!require_unlocked(req)) return ESP_OK;

  config_lock(0);
  bool available = strlen(g_config.fw_url) > 0;
  config_unlock();
  if (!available) return send_error(req, 400, "Tidak ada firmware cloud untuk perangkat ini");
  if (!ota_cloud_start()) {
    return send_error(req, 409, g_ota_error[0] ? g_ota_error : "OTA tidak bisa dimulai");
  }
  return send_ok(req, "Download firmware dimulai");
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------
static esp_err_t factory_reset_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;

  char *body = read_body(req, 1024);
  if (!body) return send_error(req, 400, "Body JSON tidak valid");
  cJSON *doc = cJSON_Parse(body);
  free(body);
  if (!doc) return send_error(req, 400, "JSON tidak valid");
  const char *confirm = get_str(doc, "confirm", "");
  bool ok = strcmp(confirm, "RESET") == 0;
  cJSON_Delete(doc);
  if (!ok) return send_error(req, 400, "Ketik RESET untuk konfirmasi");

  log_add("warn", "factory reset diminta oleh admin");
  s_factory_reset_at = millis_now() + 800UL;
  return send_ok(req, "Factory reset. Alat reboot ke mode registrasi.");
}

static esp_err_t reboot_handler(httpd_req_t *req) {
  if (!require_auth(req)) return ESP_OK;
  if (!require_csrf(req)) return ESP_OK;
  log_add("warn", "reboot diminta oleh admin");
  schedule_reboot(800);
  return send_ok(req, "Rebooting");
}

// ---------------------------------------------------------------------------
// Provisioning / captive portal
// ---------------------------------------------------------------------------
static esp_err_t scan_handler(httpd_req_t *req) {
  if (!g_ap_mode) return send_error(req, 400, "Scan hanya tersedia di mode registrasi");

  wifi_scan_config_t scan_cfg = {0};
  scan_cfg.show_hidden = false;
  esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
  if (err != ESP_OK) return send_error(req, 500, "Scan Wi-Fi gagal");

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddArrayToObject(root, "networks");
    return send_json(req, root);
  }

  wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
  if (!records) return send_error(req, 500, "out of memory");
  esp_wifi_scan_get_ap_records(&ap_count, records);

  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "networks");
  char seen[40][33];
  int seen_count = 0;

  for (int i = 0; i < ap_count && seen_count < 40; i++) {
    if (records[i].ssid[0] == 0) continue;
    bool duplicate = false;
    for (int j = 0; j < seen_count; j++) {
      if (strcmp(seen[j], (const char *)records[i].ssid) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    snprintf(seen[seen_count], sizeof(seen[seen_count]), "%s", (const char *)records[i].ssid);
    seen_count++;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", (const char *)records[i].ssid);
    cJSON_AddNumberToObject(o, "rssi", records[i].rssi);
    cJSON_AddBoolToObject(o, "secure", records[i].authmode != WIFI_AUTH_OPEN);
    cJSON_AddItemToArray(arr, o);
  }

  free(records);
  esp_wifi_clear_ap_list();
  return send_json(req, root);
}

static esp_err_t provision_handler(httpd_req_t *req) {
  if (!g_ap_mode) return send_error(req, 400, "Alat tidak dalam mode registrasi");
  if (!require_csrf(req)) return ESP_OK;

  char *body = read_body(req, 4096);
  if (!body) return send_error(req, 400, "Body JSON tidak valid");
  cJSON *doc = cJSON_Parse(body);
  free(body);
  if (!doc) return send_error(req, 400, "JSON tidak valid");

  const char *email = get_str(doc, "email", "");
  const char *ssid = get_str(doc, "ssid", "");
  const char *password = get_str(doc, "password", "");
  const char *admin_password = get_str(doc, "admin_password", "");
  const char *api_base = get_str(doc, "api_base", "");

  char error[160] = {0};
  char message[160] = {0};
  bool ok = provisioning_provision(email, ssid, password, admin_password, api_base, error,
                                    sizeof(error), message, sizeof(message));
  cJSON_Delete(doc);

  if (!ok) return send_error(req, 400, error);
  return send_ok(req, message);
}

// ---------------------------------------------------------------------------
// Not found / captive portal
// ---------------------------------------------------------------------------
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  if (g_ap_mode) {
    char location[40] = "http://192.168.4.1/";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
  }
  if (strncmp(req->uri, "/api/", 5) == 0) {
    return send_error(req, 404, "not found");
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void web_init(void) {
  random_hex(s_csrf, 16);
  memset(s_sessions, 0, sizeof(s_sessions));

  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = "storage",
    .max_files = 8,
    .format_if_mount_failed = true,
  };
  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gagal mount SPIFFS: %s", esp_err_to_name(err));
  } else {
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS %u/%u byte terpakai", (unsigned)used, (unsigned)total);
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 32;
  config.stack_size = WEB_TASK_STACK;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;

  if (httpd_start(&s_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "gagal memulai HTTP server");
    return;
  }

  static const httpd_uri_t uris[] = {
    {.uri = "/", .method = HTTP_GET, .handler = root_handler},
    {.uri = "/setup", .method = HTTP_GET, .handler = setup_handler},
    {.uri = "/settings", .method = HTTP_GET, .handler = settings_handler},
    {.uri = "/login", .method = HTTP_GET, .handler = login_page_handler},
    {.uri = "/locked", .method = HTTP_GET, .handler = locked_page_handler},
    {.uri = "/style.css", .method = HTTP_GET, .handler = css_handler},
    {.uri = "/app.js", .method = HTTP_GET, .handler = js_handler},
    {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler},
    {.uri = "/api/system", .method = HTTP_GET, .handler = system_handler},
    {.uri = "/api/sensors", .method = HTTP_GET, .handler = sensors_handler},
    {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler},
    {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler},
    {.uri = "/api/login", .method = HTTP_POST, .handler = login_handler},
    {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler},
    {.uri = "/api/alarm/ack", .method = HTTP_POST, .handler = alarm_ack_handler},
    {.uri = "/api/cloud/sync", .method = HTTP_POST, .handler = cloud_sync_handler},
    {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler},
    {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler},
    {.uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler},
    {.uri = "/api/ota/cloud", .method = HTTP_POST, .handler = ota_cloud_handler},
    {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler},
    {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
    {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
    {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_handler},
  };

  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_server, &uris[i]);
  }
  httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);

  log_add("info", "web server aktif di port 80");
}

void web_loop(void) {
  if (s_factory_reset_at != 0 && time_reached(s_factory_reset_at)) {
    s_factory_reset_at = 0;
    log_add("warn", "menghapus konfigurasi dan reboot");
    storage_clear();
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
  }
}
