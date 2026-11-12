#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "storage";
static nvs_handle_t s_nvs;
static bool s_ready = false;

static void copy_string(char *dst, size_t len, const char *src) {
  if (!dst || len == 0) return;
  if (!src) {
    dst[0] = 0;
    return;
  }
  strncpy(dst, src, len - 1);
  dst[len - 1] = 0;
}

void storage_set_defaults(void) {
  memset(&g_config, 0, sizeof(g_config));
  g_config.magic = CONFIG_MAGIC;
  g_config.version = CONFIG_VERSION;

  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_serial, sizeof(g_serial), "MG-%02X%02X%02X", mac[3], mac[4], mac[5]);
  snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s%02X%02X", AP_PREFIX, mac[4], mac[5]);
  snprintf(g_ap_pass, sizeof(g_ap_pass), "%s%02X%02X", AP_PASS_PREFIX, mac[4], mac[5]);

  copy_string(g_config.serial, sizeof(g_config.serial), g_serial);
  copy_string(g_config.api_base, sizeof(g_config.api_base), DEFAULT_API_BASE);
  password_hash(DEFAULT_ADMIN_PASS, g_config.admin_pass, sizeof(g_config.admin_pass));
  copy_string(g_config.ota_pass, sizeof(g_config.ota_pass), DEFAULT_OTA_PASS);
  copy_string(g_config.fw_version, sizeof(g_config.fw_version), FW_VERSION);
  copy_string(g_config.plan, sizeof(g_config.plan), "basic");

  g_config.active_sensors = DEFAULT_ACTIVE_SENSORS;
  g_config.default_active_sensors = DEFAULT_DEFAULT_ACTIVE_SENSORS;
  g_config.lock_policy = DEFAULT_LOCK_POLICY;
  g_config.offline_action = DEFAULT_OFFLINE_ACTION;
  g_config.grace_days = DEFAULT_GRACE_DAYS;
  g_config.allow_local_override = DEFAULT_ALLOW_LOCAL_OVERRIDE;
  g_config.auto_ota = DEFAULT_AUTO_OTA;
  g_config.quota_expires = 0;
  g_config.last_sync = 0;

  for (int i = 0; i < SENSOR_COUNT; i++) {
    snprintf(g_config.labels[i], sizeof(g_config.labels[i]), "Sensor %d", i + 1);
    g_config.thresholds[i].warn_low = DEFAULT_WARN_LOW;
    g_config.thresholds[i].alarm_low = DEFAULT_ALARM_LOW;
    g_config.thresholds[i].warn_high = DEFAULT_WARN_HIGH;
    g_config.thresholds[i].alarm_high = DEFAULT_ALARM_HIGH;
    g_config.thresholds[i].hysteresis = DEFAULT_HYSTERESIS;
    copy_string(g_config.thresholds[i].unit, sizeof(g_config.thresholds[i].unit), DEFAULT_UNIT);
  }
}

void storage_init(void) {
  esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &s_nvs);
  s_ready = (err == ESP_OK);
  if (!s_ready) {
    ESP_LOGE(TAG, "gagal membuka NVS: %s", esp_err_to_name(err));
  }
}

bool storage_load(void) {
  storage_set_defaults();
  if (!s_ready) return false;

  size_t len = 0;
  esp_err_t err = nvs_get_blob(s_nvs, "cfg", NULL, &len);
  if (err != ESP_OK || len != sizeof(DeviceConfig)) {
    if (len != 0) ESP_LOGW(TAG, "ukuran config tidak cocok: %u", (unsigned)len);
    return false;
  }

  DeviceConfig loaded;
  err = nvs_get_blob(s_nvs, "cfg", &loaded, &len);
  if (err != ESP_OK) return false;
  if (loaded.magic != CONFIG_MAGIC || loaded.version != CONFIG_VERSION) {
    ESP_LOGW(TAG, "magic/version config tidak cocok");
    return false;
  }

  g_config = loaded;
  g_config.email[sizeof(g_config.email) - 1] = 0;
  g_config.ssid[sizeof(g_config.ssid) - 1] = 0;
  g_config.wifi_pass[sizeof(g_config.wifi_pass) - 1] = 0;
  g_config.admin_pass[sizeof(g_config.admin_pass) - 1] = 0;
  g_config.device_secret[sizeof(g_config.device_secret) - 1] = 0;
  g_config.serial[sizeof(g_config.serial) - 1] = 0;
  g_config.api_base[sizeof(g_config.api_base) - 1] = 0;
  g_config.ota_pass[sizeof(g_config.ota_pass) - 1] = 0;
  g_config.fw_version[sizeof(g_config.fw_version) - 1] = 0;
  g_config.fw_available_version[sizeof(g_config.fw_available_version) - 1] = 0;
  g_config.fw_url[sizeof(g_config.fw_url) - 1] = 0;
  g_config.fw_sha[sizeof(g_config.fw_sha) - 1] = 0;
  g_config.plan[sizeof(g_config.plan) - 1] = 0;

  if (g_config.active_sensors < 1 || g_config.active_sensors > SENSOR_COUNT) {
    g_config.active_sensors = DEFAULT_ACTIVE_SENSORS;
  }
  if (g_config.default_active_sensors < 1 || g_config.default_active_sensors > SENSOR_COUNT) {
    g_config.default_active_sensors = DEFAULT_DEFAULT_ACTIVE_SENSORS;
  }
  if (g_config.grace_days > 365) g_config.grace_days = DEFAULT_GRACE_DAYS;

  copy_string(g_serial, sizeof(g_serial), g_config.serial);
  if (strlen(g_config.api_base) == 0) {
    copy_string(g_config.api_base, sizeof(g_config.api_base), DEFAULT_API_BASE);
  }

  g_provisioned = strlen(g_config.email) > 0 && strlen(g_config.device_secret) > 0;
  return true;
}

bool storage_save(void) {
  if (!s_ready) return false;
  g_config.magic = CONFIG_MAGIC;
  g_config.version = CONFIG_VERSION;
  esp_err_t err = nvs_set_blob(s_nvs, "cfg", &g_config, sizeof(g_config));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_set_blob gagal: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_commit(s_nvs);
  return err == ESP_OK;
}

void storage_clear(void) {
  if (!s_ready) return;
  nvs_erase_all(s_nvs);
  nvs_commit(s_nvs);
  nvs_close(s_nvs);
  s_ready = false;
}
