#include "net_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "log_buffer.h"

#if !ALLOW_INSECURE_TLS
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "net";

#define CONNECTED_BIT BIT0
#define FAIL_BIT BIT1

static EventGroupHandle_t s_events = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_connected = false;
static bool s_got_ip = false;
static bool s_wifi_started = false;
static bool s_time_started = false;
static uint32_t s_last_reconnect = 0;
static uint32_t s_last_internet_check = 0;
static bool s_internet_known = false;
static bool s_internet_ok = false;
static char s_ip[16] = "0.0.0.0";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_CONNECTED) {
      s_connected = true;
      log_add("info", "Wi-Fi terhubung ke AP");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
      s_connected = false;
      s_got_ip = false;
      strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);
      xEventGroupSetBits(s_events, FAIL_BIT);
      log_add("warn", "Wi-Fi terputus");
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_connected = true;
    s_got_ip = true;
    xEventGroupSetBits(s_events, CONNECTED_BIT);
    log_add("info", "Wi-Fi dapat IP %s", s_ip);
  }
}

void net_init(void) {
  s_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();
  esp_netif_set_hostname(s_sta_netif, "medgas-alarm");

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  s_wifi_started = true;

  setenv("TZ", "WIB-7", 1);
  tzset();

  ESP_LOGI(TAG, "net manager siap");
}

bool net_connect_to(const char *ssid, const char *pass, uint32_t timeout_ms) {
  if (!ssid || strlen(ssid) == 0) {
    log_add("warn", "SSID kosong, tidak bisa konek");
    return false;
  }
  if (!s_wifi_started) return false;

  wifi_mode_t mode = g_ap_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

  wifi_config_t wc;
  memset(&wc, 0, sizeof(wc));
  strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  wc.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

  xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));
  xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);
  s_connected = false;
  s_got_ip = false;
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    log_add("error", "esp_wifi_connect gagal: %s", esp_err_to_name(err));
    return false;
  }
  log_add("info", "konek Wi-Fi ke %s", ssid);

  // Wait for an IP address, not just the L2 association. The disconnect event
  // from esp_wifi_disconnect() above may arrive asynchronously, so poll the
  // IP flag instead of relying on the event-group bits.
  uint32_t start = millis_now();
  while (!s_got_ip && (millis_now() - start) < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (s_got_ip) {
    net_sync_time();
    return true;
  }

  log_add("error", "gagal konek Wi-Fi (timeout)");
  esp_wifi_disconnect();
  if (g_ap_mode) {
    esp_wifi_set_mode(WIFI_MODE_AP);
  }
  return false;
}

bool net_connect_sta(uint32_t timeout_ms) {
  return net_connect_to(g_config.ssid, g_config.wifi_pass, timeout_ms);
}

void net_loop(void) {
  if (g_ap_mode) return;
  if (net_is_connected()) return;
  if (millis_now() - s_last_reconnect < WIFI_RECONNECT_INTERVAL_MS) return;

  s_last_reconnect = millis_now();
  log_add("warn", "Wi-Fi terputus, mencoba reconnect");
  esp_wifi_connect();
}

bool net_is_connected(void) {
  return s_connected && s_got_ip;
}

int net_rssi(void) {
  if (!net_is_connected()) return 0;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
  return 0;
}

const char *net_ip(void) {
  return s_ip;
}

const char *net_ssid(void) {
  if (net_is_connected()) {
    static char ssid[33];
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      strncpy(ssid, (const char *)ap.ssid, sizeof(ssid) - 1);
      ssid[sizeof(ssid) - 1] = 0;
      if (ssid[0]) return ssid;
    }
  }
  return g_config.ssid;
}

void net_sync_time(void) {
  if (!net_is_connected()) return;
  if (time_valid()) return;

  if (!s_time_started) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    s_time_started = true;
  }

  uint32_t start = millis_now();
  while (!time_valid() && (millis_now() - start) < NTP_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (time_valid()) {
    log_add("info", "NTP sinkron, epoch %lu", (unsigned long)now_epoch());
  } else {
    log_add("warn", "NTP belum tersedia");
  }
}

bool net_check_internet(void) {
  if (!net_is_connected()) return false;
  if (s_internet_known && millis_now() - s_last_internet_check < 30000UL) {
    return s_internet_ok;
  }

  char url[192];
  snprintf(url, sizeof(url), "%s/api/v1/health", g_config.api_base);

  esp_http_client_config_t cfg = {
    .url = url,
    .timeout_ms = 5000,
    .keep_alive_enable = false,
  };
#if ALLOW_INSECURE_TLS
  cfg.skip_cert_common_name_check = true;
#else
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return false;

  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_err_t err = esp_http_client_perform(client);
  int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
  esp_http_client_cleanup(client);

  s_internet_known = true;
  s_last_internet_check = millis_now();
  s_internet_ok = (status == 200);
  return s_internet_ok;
}
