#include "provisioning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "app_state.h"
#include "cloud_sync.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net_manager.h"
#include "storage.h"

static const char *TAG = "provision";
static bool s_recovery = false;
static bool s_dns_started = false;
static int s_dns_sock = -1;

static void dns_task(void *arg) {
  (void)arg;
  uint8_t buf[512];
  uint8_t resp[512];

  for (;;) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(s_dns_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
    if (n < 12) continue;

    // Only answer standard single-question A queries.
    uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
    if (qdcount < 1) continue;

    int q_end = 12;
    while (q_end < n) {
      uint8_t label = buf[q_end];
      if (label == 0) {
        q_end++;
        break;
      }
      if ((label & 0xC0) == 0xC0) {
        q_end += 2;
        break;
      }
      q_end += label + 1;
    }
    if (q_end + 4 > n) continue;
    int question_end = q_end + 4;
    if (question_end + 16 > (int)sizeof(resp)) continue;

    memcpy(resp, buf, (size_t)question_end);
    resp[2] = 0x81;  // QR=1, RD copied, RA=1
    resp[3] = 0x80;  // no error
    resp[6] = 0x00;
    resp[7] = 0x01;  // answer count
    resp[8] = 0x00;
    resp[9] = 0x00;
    resp[10] = 0x00;
    resp[11] = 0x00;

    int p = question_end;
    resp[p++] = 0xC0;  // pointer to question name
    resp[p++] = 0x0C;
    resp[p++] = 0x00;
    resp[p++] = 0x01;  // A
    resp[p++] = 0x00;
    resp[p++] = 0x01;  // IN
    resp[p++] = 0x00;
    resp[p++] = 0x00;
    resp[p++] = 0x00;
    resp[p++] = 0x3C;  // TTL 60
    resp[p++] = 0x00;
    resp[p++] = 0x04;

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info;
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
      memcpy(&resp[p], &ip_info.ip.addr, 4);
    } else {
      resp[p + 0] = 192;
      resp[p + 1] = 168;
      resp[p + 2] = 4;
      resp[p + 3] = 1;
    }
    p += 4;

    sendto(s_dns_sock, resp, p, 0, (struct sockaddr *)&from, from_len);
  }
}

void provisioning_init(void) {
  s_recovery = false;
}

static void start_dns(void) {
  if (s_dns_started) return;

  s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_dns_sock < 0) {
    ESP_LOGE(TAG, "gagal membuat DNS socket");
    return;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "gagal bind DNS socket");
    close(s_dns_sock);
    s_dns_sock = -1;
    return;
  }

  xTaskCreate(dns_task, "dns-captive", 4096, NULL, 3, NULL);
  s_dns_started = true;
}

void provisioning_start_ap(bool recovery) {
  s_recovery = recovery;
  g_ap_mode = true;

  if (strlen(g_ap_ssid) == 0) {
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s%04X", AP_PREFIX, (unsigned)(esp_random() & 0xFFFF));
  }
  if (strlen(g_ap_pass) < 8) {
    snprintf(g_ap_pass, sizeof(g_ap_pass), "%s%04X", AP_PASS_PREFIX,
             (unsigned)(esp_random() & 0xFFFF));
  }

  wifi_config_t ap;
  memset(&ap, 0, sizeof(ap));
  strncpy((char *)ap.ap.ssid, g_ap_ssid, sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = strlen(g_ap_ssid);
  strncpy((char *)ap.ap.password, g_ap_pass, sizeof(ap.ap.password) - 1);
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  // net_init() already started the driver, so a non-OK result here is not
  // necessarily fatal; log it and keep the AP configuration in place.
  esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
  }

  start_dns();

  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_ip_info_t ip_info;
  char ip[16] = "192.168.4.1";
  if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
  }

  log_add("info", "AP %s aktif, password %s, IP %s", g_ap_ssid, g_ap_pass, ip);
  ESP_LOGI(TAG, "AP SSID=%s PASS=%s IP=%s", g_ap_ssid, g_ap_pass, ip);
  if (recovery) {
    log_add("warn", "mode pemulihan: perbaiki Wi-Fi lewat captive portal");
  }
}

void provisioning_stop_ap(void) {
  g_ap_mode = false;
  esp_wifi_set_mode(WIFI_MODE_STA);
}

void provisioning_loop(void) {
  // The DNS responder runs in its own task; nothing to do here.
}

bool provisioning_is_recovery(void) {
  return s_recovery;
}

static bool valid_email(const char *email) {
  size_t len = strlen(email);
  return len >= 5 && len <= 95 && strchr(email, '@') != NULL;
}

static void return_to_ap(void) {
  esp_wifi_disconnect();
  if (g_ap_mode) esp_wifi_set_mode(WIFI_MODE_AP);
}

bool provisioning_provision(const char *email, const char *ssid, const char *pass,
                             const char *admin_password, const char *api_base,
                             char *error, size_t error_len,
                             char *message, size_t message_len) {
  if (error && error_len) error[0] = 0;
  if (message && message_len) message[0] = 0;

  if (!g_ap_mode) {
    snprintf(error, error_len, "Alat tidak dalam mode registrasi");
    return false;
  }

  char target_email[96];
  snprintf(target_email, sizeof(target_email), "%s", email ? email : "");

  if (g_provisioned) {
    snprintf(target_email, sizeof(target_email), "%s", g_config.email);
    char hash[65];
    password_hash(admin_password ? admin_password : "", hash, sizeof(hash));
    if (strcmp(hash, g_config.admin_pass) != 0) {
      snprintf(error, error_len, "Password admin salah");
      return false;
    }
  } else {
    if (!valid_email(target_email)) {
      snprintf(error, error_len, "Email ID tidak valid");
      return false;
    }
    if (!admin_password || strlen(admin_password) < 8) {
      snprintf(error, error_len, "Password admin minimal 8 karakter");
      return false;
    }
  }

  if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
    snprintf(error, error_len, "SSID tidak valid");
    return false;
  }

  char target_api[128];
  snprintf(target_api, sizeof(target_api), "%s", (api_base && api_base[0]) ? api_base
                                                                          : g_config.api_base);
  if (target_api[0] == 0) snprintf(target_api, sizeof(target_api), "%s", DEFAULT_API_BASE);
  if (strncmp(target_api, "http://", 7) != 0 && strncmp(target_api, "https://", 8) != 0) {
    snprintf(error, error_len, "Alamat server harus diawali http:// atau https://");
    return false;
  }
  size_t api_len = strlen(target_api);
  while (api_len > 0 && target_api[api_len - 1] == '/') target_api[--api_len] = 0;

  log_add("info", "registrasi: email=%s ssid=%s server=%s", target_email, ssid, target_api);

  if (!net_connect_to(ssid, pass, WIFI_CONNECT_TIMEOUT_MS)) {
    return_to_ap();
    snprintf(error, error_len, "Gagal terhubung ke Wi-Fi. Periksa nama jaringan dan password.");
    return false;
  }

  char *response = NULL;
  if (!cloud_provision(target_api, target_email, g_serial, &response)) {
    return_to_ap();
    snprintf(error, error_len,
             "Tidak bisa menghubungi server vendor di %s. Periksa alamat server dan internet.",
             target_api);
    free(response);
    return false;
  }

  cJSON *doc = cJSON_Parse(response);
  free(response);
  if (!doc) {
    return_to_ap();
    snprintf(error, error_len, "Respons server tidak valid");
    return false;
  }

  cJSON *ok = cJSON_GetObjectItemCaseSensitive(doc, "ok");
  if (!cJSON_IsTrue(ok)) {
    cJSON *err = cJSON_GetObjectItemCaseSensitive(doc, "error");
    snprintf(error, error_len, "%s",
             cJSON_IsString(err) ? err->valuestring : "Registrasi ditolak server");
    cJSON_Delete(doc);
    return_to_ap();
    return false;
  }

  cJSON *secret = cJSON_GetObjectItemCaseSensitive(doc, "device_secret");
  if (!cJSON_IsString(secret) || strlen(secret->valuestring) < 32) {
    cJSON_Delete(doc);
    return_to_ap();
    snprintf(error, error_len, "Server tidak mengirim device secret");
    return false;
  }

  config_lock(0);
  snprintf(g_config.email, sizeof(g_config.email), "%s", target_email);
  snprintf(g_config.ssid, sizeof(g_config.ssid), "%s", ssid);
  snprintf(g_config.wifi_pass, sizeof(g_config.wifi_pass), "%s", pass ? pass : "");
  snprintf(g_config.api_base, sizeof(g_config.api_base), "%s", target_api);
  snprintf(g_config.device_secret, sizeof(g_config.device_secret), "%s", secret->valuestring);
  snprintf(g_config.serial, sizeof(g_config.serial), "%s", g_serial);
  snprintf(g_config.fw_version, sizeof(g_config.fw_version), "%s", FW_VERSION);
  password_hash(admin_password ? admin_password : "", g_config.admin_pass,
                sizeof(g_config.admin_pass));
  config_unlock();

  cJSON *config = cJSON_GetObjectItemCaseSensitive(doc, "config");
  cloud_apply_config(config, false);

  config_lock(0);
  storage_save();
  g_provisioned = true;
  config_unlock();

  g_cloud.online = true;
  g_cloud.ever_synced = true;
  g_cloud.last_sync = now_epoch();
  g_cloud.last_error[0] = 0;

  cJSON_Delete(doc);

  snprintf(message, message_len, "Registrasi berhasil. Alat akan reboot ke dashboard.");
  log_add("info", "registrasi selesai untuk %s", target_email);
  schedule_reboot(3000);
  return true;
}
