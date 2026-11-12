#include "cloud_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "app_state.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "net_manager.h"
#include "storage.h"

static const char *TAG = "cloud";

static TaskHandle_t s_cloud_task = NULL;
static volatile bool s_cloud_running = false;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} strbuf_t;

static bool sb_append(strbuf_t *sb, const char *data, size_t len) {
  if (sb->len + len + 1 > sb->cap) {
    size_t cap = sb->cap ? sb->cap : 512;
    while (cap < sb->len + len + 1) cap *= 2;
    char *next = realloc(sb->data, cap);
    if (!next) return false;
    sb->data = next;
    sb->cap = cap;
  }
  memcpy(sb->data + sb->len, data, len);
  sb->len += len;
  sb->data[sb->len] = 0;
  return true;
}

static void sha256_hex_buf(const char *data, char *out) {
  uint8_t hash[32];
  mbedtls_sha256((const unsigned char *)data, strlen(data), hash, 0);
  static const char *hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[(hash[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[hash[i] & 0x0F];
  }
  out[64] = 0;
}

static void hmac_hex(const char *secret, const char *data, char *out) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t digest[32];
  mbedtls_md_hmac(info, (const unsigned char *)secret, strlen(secret),
                  (const unsigned char *)data, strlen(data), digest);
  static const char *hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[(digest[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  out[64] = 0;
}

static void random_nonce(char *out, size_t len) {
  snprintf(out, len, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
}

typedef struct {
  char *signature;
} response_capture_t;

// Response headers are only visible through the HTTP event callback.
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  response_capture_t *cap = (response_capture_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_HEADER && cap && evt->header_key && evt->header_value) {
    if (strcasecmp(evt->header_key, "X-Signature") == 0) {
      size_t n = strlen(evt->header_value) + 1;
      char *copy = malloc(n);
      if (copy) {
        memcpy(copy, evt->header_value, n);
        free(cap->signature);
        cap->signature = copy;
      }
    }
  }
  return ESP_OK;
}

static int http_request(const char *method, const char *url, const char *body,
                        const char *email, const char *timestamp, const char *nonce,
                        const char *signature, char **response_out, char **sig_out) {
  if (response_out) *response_out = NULL;
  if (sig_out) *sig_out = NULL;

  response_capture_t capture = {0};
  esp_http_client_config_t cfg = {
    .url = url,
    .timeout_ms = 9000,
    .event_handler = http_event_handler,
    .user_data = &capture,
    .keep_alive_enable = false,
  };
#if ALLOW_INSECURE_TLS
  cfg.skip_cert_common_name_check = true;
#else
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return -1;

  bool is_post = strcmp(method, "POST") == 0;
  esp_http_client_set_method(client, is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
  if (email && email[0]) {
    esp_http_client_set_header(client, "X-Device-Email", email);
    esp_http_client_set_header(client, "X-Timestamp", timestamp);
    esp_http_client_set_header(client, "X-Nonce", nonce);
    esp_http_client_set_header(client, "X-Signature", signature);
  }

  int body_len = body ? (int)strlen(body) : 0;
  if (is_post) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, body_len);
  }

  esp_err_t err = esp_http_client_open(client, body_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "open %s gagal: %s", url, esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return -1;
  }

  if (body_len > 0) {
    int written = esp_http_client_write(client, body, body_len);
    if (written != body_len) {
      ESP_LOGE(TAG, "write body gagal (%d/%d)", written, body_len);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return -1;
    }
  }

  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);

  if (sig_out) {
    *sig_out = capture.signature;
    capture.signature = NULL;
  }

  strbuf_t sb = {0};
  char buf[1024];
  int read;
  while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
    if (!sb_append(&sb, buf, (size_t)read)) break;
  }
  if (!sb.data) sb_append(&sb, "", 0);

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  free(capture.signature);

  if (response_out) *response_out = sb.data;
  else free(sb.data);
  return status;
}

static int json_int(cJSON *obj, const char *key, int def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? item->valueint : def;
}

static uint32_t json_u32(cJSON *obj, const char *key, uint32_t def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : def;
}

static const char *json_str(cJSON *obj, const char *key, const char *def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : def;
}

static float json_float(cJSON *obj, const char *key, float def) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

static void copy_str(char *dst, size_t len, const char *src) {
  if (!dst || len == 0) return;
  if (!src) {
    dst[0] = 0;
    return;
  }
  strncpy(dst, src, len - 1);
  dst[len - 1] = 0;
}

static void apply_quota(cJSON *quota) {
  if (!cJSON_IsObject(quota)) return;
  copy_str(g_config.plan, sizeof(g_config.plan), json_str(quota, "plan", "basic"));
  g_config.quota_expires = json_u32(quota, "expires_epoch", 0);
}

bool cloud_apply_config(cJSON *config, bool save) {
  if (!cJSON_IsObject(config)) return false;
  config_lock(0);

  int active = json_int(config, "active_sensors", (int)g_config.active_sensors);
  if (active < 1) active = 1;
  if (active > SENSOR_COUNT) active = SENSOR_COUNT;
  bool changed_active = (uint8_t)active != g_config.active_sensors;
  g_config.active_sensors = (uint8_t)active;

  cJSON *labels = cJSON_GetObjectItemCaseSensitive(config, "sensor_labels");
  if (cJSON_IsArray(labels)) {
    int i = 0;
    cJSON *v;
    cJSON_ArrayForEach(v, labels) {
      if (i >= SENSOR_COUNT) break;
      if (cJSON_IsString(v) && v->valuestring[0]) {
        copy_str(g_config.labels[i], sizeof(g_config.labels[i]), v->valuestring);
      }
      i++;
    }
  }

  cJSON *thresholds = cJSON_GetObjectItemCaseSensitive(config, "thresholds");
  if (cJSON_IsArray(thresholds)) {
    int i = 0;
    cJSON *v;
    cJSON_ArrayForEach(v, thresholds) {
      if (i >= SENSOR_COUNT) break;
      if (cJSON_IsObject(v)) {
        Threshold *t = &g_config.thresholds[i];
        t->warn_low = json_float(v, "warn_low", t->warn_low);
        t->alarm_low = json_float(v, "alarm_low", t->alarm_low);
        t->warn_high = json_float(v, "warn_high", t->warn_high);
        t->alarm_high = json_float(v, "alarm_high", t->alarm_high);
        t->hysteresis = json_float(v, "hysteresis", t->hysteresis);
        const char *unit = json_str(v, "unit", NULL);
        if (unit && unit[0]) copy_str(t->unit, sizeof(t->unit), unit);
      }
      i++;
    }
  }

  const char *policy = json_str(config, "lock_policy", "");
  if (strcmp(policy, "grace") == 0) g_config.lock_policy = LOCK_GRACE;
  else if (strcmp(policy, "strict") == 0) g_config.lock_policy = LOCK_STRICT;
  else if (strcmp(policy, "provision_only") == 0) g_config.lock_policy = LOCK_PROVISION_ONLY;

  cJSON *override = cJSON_GetObjectItemCaseSensitive(config, "allow_local_override");
  if (cJSON_IsBool(override)) g_config.allow_local_override = cJSON_IsTrue(override);

  cJSON *auto_ota = cJSON_GetObjectItemCaseSensitive(config, "auto_ota");
  if (cJSON_IsBool(auto_ota)) g_config.auto_ota = cJSON_IsTrue(auto_ota);

  int grace = json_int(config, "grace_days", (int)g_config.grace_days);
  if (grace < 0) grace = 0;
  if (grace > 365) grace = 365;
  g_config.grace_days = (uint16_t)grace;

  const char *offline = json_str(config, "offline_action", "");
  if (strcmp(offline, "fallback_default") == 0) g_config.offline_action = OFFLINE_FALLBACK_DEFAULT;
  else if (strcmp(offline, "lock_ui") == 0) g_config.offline_action = OFFLINE_LOCK_UI;
  else if (strcmp(offline, "keep_last") == 0) g_config.offline_action = OFFLINE_KEEP_LAST;

  int default_active = json_int(config, "default_active_sensors",
                                (int)g_config.default_active_sensors);
  if (default_active < 1) default_active = 1;
  if (default_active > SENSOR_COUNT) default_active = SENSOR_COUNT;
  g_config.default_active_sensors = (uint8_t)default_active;

  apply_quota(cJSON_GetObjectItemCaseSensitive(config, "quota"));

  cJSON *fw = cJSON_GetObjectItemCaseSensitive(config, "firmware");
  g_config.fw_url[0] = 0;
  g_config.fw_sha[0] = 0;
  g_config.fw_available_version[0] = 0;
  g_config.fw_size = 0;
  if (cJSON_IsObject(fw)) {
    const char *version = json_str(fw, "version", "");
    const char *url = json_str(fw, "url", "");
    const char *sha = json_str(fw, "sha256", "");
    if (version[0] && strcmp(version, FW_VERSION) != 0) {
      copy_str(g_config.fw_available_version, sizeof(g_config.fw_available_version), version);
      copy_str(g_config.fw_url, sizeof(g_config.fw_url), url);
      copy_str(g_config.fw_sha, sizeof(g_config.fw_sha), sha);
      g_config.fw_size = json_u32(fw, "size", 0);
    }
  }

  if (changed_active) {
    log_add("info", "kuota sensor diupdate menjadi %u", (unsigned)g_config.active_sensors);
  }
  if (save) storage_save();
  config_unlock();
  return true;
}

static void set_locked(bool locked, const char *reason) {
  bool changed = g_ui_locked != locked;
  g_ui_locked = locked;
  if (locked) {
    copy_str(g_lock_reason, sizeof(g_lock_reason), reason ? reason : "Dashboard terkunci");
  } else {
    g_lock_reason[0] = 0;
  }
  if (changed) {
    log_add(locked ? "warn" : "info", "UI %s: %s", locked ? "dikunci" : "dibuka",
            g_lock_reason);
  }
}

void cloud_apply_policy(void) {
  uint32_t now = now_epoch();
  if (now == 0) {
    if (g_config.lock_policy == LOCK_STRICT && !g_cloud.online) {
      set_locked(true, "Mode strict: internet/NTP tidak tersedia");
    } else {
      set_locked(false, "");
    }
    return;
  }

  if (g_config.lock_policy == LOCK_STRICT && !g_cloud.online) {
    set_locked(true, "Mode strict: alat membutuhkan koneksi internet");
    return;
  }

  bool quota_expired = g_config.quota_expires > 0 && now > g_config.quota_expires;
  bool sync_stale = g_config.lock_policy == LOCK_GRACE && g_cloud.last_sync > 0 &&
                    now > g_cloud.last_sync + (uint32_t)g_config.grace_days * 86400UL;

  if (!quota_expired && !sync_stale) {
    set_locked(false, "");
    return;
  }

  uint32_t deadline = g_config.quota_expires;
  if (sync_stale && g_cloud.last_sync > deadline) deadline = g_cloud.last_sync;
  if (deadline > 0 && now <= deadline + (uint32_t)g_config.grace_days * 86400UL) {
    set_locked(false, "");
    return;
  }

  switch (g_config.offline_action) {
    case OFFLINE_FALLBACK_DEFAULT:
      config_lock(0);
      g_config.active_sensors = g_config.default_active_sensors;
      storage_save();
      config_unlock();
      set_locked(false, "");
      log_add("warn", "offline: fallback ke %u sensor", (unsigned)g_config.active_sensors);
      break;
    case OFFLINE_LOCK_UI:
      set_locked(true, "Kuota kedaluwarsa / offline melewati grace period");
      break;
    case OFFLINE_KEEP_LAST:
    default:
      set_locked(false, "");
      break;
  }
}

bool cloud_provision(const char *api_base, const char *email, const char *serial,
                     char **response_out) {
  if (response_out) *response_out = NULL;

  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "email", email);
  cJSON_AddStringToObject(body, "serial", serial);
  cJSON_AddStringToObject(body, "hw_rev", HW_REV);
  cJSON_AddStringToObject(body, "fw_version", FW_VERSION);
  char *payload = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (!payload) return false;

  char url[192];
  snprintf(url, sizeof(url), "%s/api/v1/provision", api_base);
  char *response = NULL;
  int status = http_request("POST", url, payload, NULL, NULL, NULL, NULL, &response, NULL);
  free(payload);

  if (status != 200) {
    log_add("error", "provisioning cloud gagal (HTTP %d): %s", status,
            response ? response : "");
    free(response);
    return false;
  }

  if (response_out) *response_out = response;
  else free(response);
  log_add("info", "provisioning cloud sukses");
  return true;
}

static void cloud_sync_internal(void) {
  if (!g_provisioned) return;
  if (!net_is_connected()) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "Wi-Fi tidak terhubung");
    cloud_apply_policy();
    return;
  }
  if (!time_valid()) net_sync_time();
  if (!time_valid()) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "waktu NTP belum tersedia");
    log_add("warn", "sync ditunda: NTP belum tersedia");
    cloud_apply_policy();
    return;
  }

  char email[sizeof(g_config.email)];
  char secret[sizeof(g_config.device_secret)];
  char api_base[sizeof(g_config.api_base)];
  config_lock(0);
  copy_str(email, sizeof(email), g_config.email);
  copy_str(secret, sizeof(secret), g_config.device_secret);
  copy_str(api_base, sizeof(api_base), g_config.api_base);
  config_unlock();

  char path[192];
  snprintf(path, sizeof(path), "/api/v1/devices/%s/config", email);
  char url[320];
  snprintf(url, sizeof(url), "%s%s", api_base, path);
  char ts[16];
  snprintf(ts, sizeof(ts), "%lu", (unsigned long)now_epoch());
  char nonce[24];
  random_nonce(nonce, sizeof(nonce));

  char empty_hash[65];
  sha256_hex_buf("", empty_hash);
  char canonical[384];
  snprintf(canonical, sizeof(canonical), "GET\n%s\n%s\n%s", path, ts, empty_hash);
  char signature[65];
  hmac_hex(secret, canonical, signature);

  char *response = NULL;
  char *response_sig = NULL;
  int status = http_request("GET", url, NULL, email, ts, nonce, signature, &response,
                            &response_sig);
  g_cloud.last_attempt = millis_now();

  if (status != 200) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "config HTTP %d", status);
    log_add("error", "sync config gagal (HTTP %d)", status);
    free(response);
    free(response_sig);
    cloud_apply_policy();
    return;
  }

  char expected[65];
  hmac_hex(secret, response ? response : "", expected);
  if (!response_sig || strcmp(expected, response_sig) != 0) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "signature respons tidak valid");
    log_add("error", "sync ditolak: signature respons tidak valid");
    free(response);
    free(response_sig);
    cloud_apply_policy();
    return;
  }

  cJSON *doc = cJSON_Parse(response);
  free(response);
  free(response_sig);
  if (!doc) {
    g_cloud.online = false;
    snprintf(g_cloud.last_error, sizeof(g_cloud.last_error), "JSON config invalid");
    log_add("error", "JSON config invalid");
    cloud_apply_policy();
    return;
  }

  cloud_apply_config(doc, true);
  cJSON_Delete(doc);

  g_cloud.online = true;
  g_cloud.ever_synced = true;
  g_cloud.last_sync = now_epoch();
  config_lock(0);
  g_config.last_sync = g_cloud.last_sync;
  storage_save();
  config_unlock();
  g_cloud.last_error[0] = 0;
  cloud_apply_policy();
  log_add("info", "sync cloud sukses, %u sensor aktif", (unsigned)g_config.active_sensors);

  // Heartbeat is best-effort; a failure must not invalidate the config.
  cJSON *hb = cJSON_CreateObject();
  cJSON_AddStringToObject(hb, "fw_version", FW_VERSION);
  cJSON_AddNumberToObject(hb, "active_sensors", g_config.active_sensors);
  cJSON_AddNumberToObject(hb, "wifi_rssi", net_rssi());
  cJSON_AddNumberToObject(hb, "uptime", millis_now() / 1000UL);
  cJSON_AddStringToObject(hb, "alarm_state", alarm_level_name(g_alarm.state));
  cJSON_AddBoolToObject(hb, "online", true);
  char *hb_body = cJSON_PrintUnformatted(hb);
  cJSON_Delete(hb);
  if (!hb_body) return;

  char hb_path[192];
  snprintf(hb_path, sizeof(hb_path), "/api/v1/devices/%s/heartbeat", email);
  char hb_url[320];
  snprintf(hb_url, sizeof(hb_url), "%s%s", api_base, hb_path);
  char hb_ts[16];
  snprintf(hb_ts, sizeof(hb_ts), "%lu", (unsigned long)now_epoch());
  char hb_nonce[24];
  random_nonce(hb_nonce, sizeof(hb_nonce));
  char hb_hash[65];
  sha256_hex_buf(hb_body, hb_hash);
  char hb_canonical[384];
  snprintf(hb_canonical, sizeof(hb_canonical), "POST\n%s\n%s\n%s", hb_path, hb_ts, hb_hash);
  char hb_sig[65];
  hmac_hex(secret, hb_canonical, hb_sig);

  char *hb_response = NULL;
  char *hb_response_sig = NULL;
  http_request("POST", hb_url, hb_body, email, hb_ts, hb_nonce, hb_sig, &hb_response,
               &hb_response_sig);
  free(hb_body);
  free(hb_response);
  free(hb_response_sig);
}

static void cloud_task(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_cloud_running = true;
    cloud_sync_internal();
    s_cloud_running = false;
  }
}

void cloud_init(void) {
  g_cloud.online = false;
  g_cloud.ever_synced = false;
  g_cloud.last_sync = 0;
  g_cloud.last_attempt = 0;
  g_cloud.last_error[0] = 0;

  if (s_cloud_task == NULL) {
    xTaskCreate(cloud_task, "cloud-sync", 12288, NULL, 4, &s_cloud_task);
  }
}

bool cloud_sync_busy(void) {
  return s_cloud_running;
}

bool cloud_request_sync(void) {
  if (s_cloud_task == NULL) return false;
  if (s_cloud_running) return false;
  xTaskNotifyGive(s_cloud_task);
  return true;
}

void cloud_loop(void) {
  if (!g_provisioned || g_ap_mode) return;
  if (s_cloud_running) return;
  if (g_cloud.last_attempt != 0 &&
      millis_now() - g_cloud.last_attempt < CLOUD_RETRY_INTERVAL_MS) {
    return;
  }

  uint32_t now = now_epoch();
  if (g_cloud.last_sync != 0 && now != 0 &&
      now - g_cloud.last_sync < (CLOUD_SYNC_INTERVAL_MS / 1000UL)) {
    return;
  }
  cloud_request_sync();
}
