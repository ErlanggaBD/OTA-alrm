#include "ota_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "app_state.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "mbedtls/sha256.h"
#include "net_manager.h"

static const char *TAG = "ota";

static volatile bool s_busy = false;
static bool s_web_active = false;
static bool s_auto_attempted = false;
static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_ota_part = NULL;
static mbedtls_sha256_context s_sha;
static bool s_sha_active = false;
static size_t s_total = 0;
static size_t s_written = 0;
static char s_expected_sha[65];

static void set_status(const char *status, int progress) {
  snprintf(g_ota_status, sizeof(g_ota_status), "%s", status ? status : "idle");
  g_ota_progress = progress;
}

static bool is_hex64(const char *value) {
  if (!value || strlen(value) != 64) return false;
  for (int i = 0; i < 64; i++) {
    char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

static void sha_to_hex(const uint8_t *digest, char *out) {
  static const char *hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[(digest[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  out[64] = 0;
}

bool ota_busy(void) {
  return s_busy;
}

void ota_init(void) {
  set_status("idle", 0);
  g_ota_error[0] = 0;
  log_add("info", "OTA manager siap (web upload + cloud)");
}

void ota_loop(void) {
  if (s_busy) return;
  if (!g_config.auto_ota) return;
  if (s_auto_attempted) return;
  if (!g_provisioned || g_ap_mode || !net_is_connected()) return;
  if (strlen(g_config.fw_url) == 0) return;

  s_auto_attempted = true;
  log_add("info", "auto OTA ke %s", g_config.fw_available_version);
  ota_cloud_start();
}

bool ota_web_begin(size_t total, const char *expected_sha) {
  if (s_busy) {
    snprintf(g_ota_error, sizeof(g_ota_error), "OTA lain sedang berjalan");
    return false;
  }
  if (total == 0) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Ukuran firmware tidak valid");
    return false;
  }
  if (expected_sha && expected_sha[0] && !is_hex64(expected_sha)) {
    snprintf(g_ota_error, sizeof(g_ota_error), "SHA-256 header tidak valid");
    return false;
  }

  s_ota_part = esp_ota_get_next_update_partition(NULL);
  if (!s_ota_part) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak ada partisi OTA tersedia");
    return false;
  }

  esp_err_t err = esp_ota_begin(s_ota_part, total, &s_ota_handle);
  if (err != ESP_OK) {
    snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_begin: %s", esp_err_to_name(err));
    set_status("error", 0);
    log_add("error", "OTA web gagal mulai: %s", g_ota_error);
    return false;
  }

  s_total = total;
  s_written = 0;
  s_busy = true;
  s_web_active = true;
  snprintf(s_expected_sha, sizeof(s_expected_sha), "%s", expected_sha ? expected_sha : "");
  mbedtls_sha256_init(&s_sha);
  mbedtls_sha256_starts(&s_sha, 0);
  s_sha_active = true;
  set_status("uploading", 0);
  g_ota_error[0] = 0;
  log_add("warn", "OTA web dimulai (%u byte%s)", (unsigned)total,
          s_expected_sha[0] ? ", SHA-256" : "");
  return true;
}

bool ota_web_write(const uint8_t *data, size_t len) {
  if (!s_web_active || !data || len == 0) return false;

  esp_err_t err = esp_ota_write(s_ota_handle, data, len);
  if (err != ESP_OK) {
    snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_write: %s", esp_err_to_name(err));
    set_status("error", 0);
    log_add("error", "OTA web gagal menulis: %s", g_ota_error);
    ota_web_abort();
    return false;
  }
  mbedtls_sha256_update(&s_sha, data, len);
  s_written += len;
  if (s_total) g_ota_progress = (int)((s_written * 100ULL) / s_total);
  return true;
}

bool ota_web_finish(void) {
  if (!s_web_active) {
    if (g_ota_error[0] == 0) {
      snprintf(g_ota_error, sizeof(g_ota_error), "Upload firmware tidak aktif");
    }
    s_busy = false;
    return false;
  }

  s_web_active = false;
  set_status("verifying", g_ota_progress);

  uint8_t digest[32];
  mbedtls_sha256_finish(&s_sha, digest);
  mbedtls_sha256_free(&s_sha);
  s_sha_active = false;

  char actual[65];
  sha_to_hex(digest, actual);
  if (s_expected_sha[0] && strcasecmp(actual, s_expected_sha) != 0) {
    snprintf(g_ota_error, sizeof(g_ota_error), "SHA-256 firmware tidak cocok");
    esp_ota_abort(s_ota_handle);
    s_ota_handle = 0;
    set_status("error", 0);
    s_busy = false;
    log_add("error", "OTA web gagal verifikasi: %s", g_ota_error);
    return false;
  }

  esp_err_t err = esp_ota_end(s_ota_handle);
  s_ota_handle = 0;
  if (err != ESP_OK) {
    snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_end: %s", esp_err_to_name(err));
    set_status("error", 0);
    s_busy = false;
    log_add("error", "OTA web gagal verifikasi: %s", g_ota_error);
    return false;
  }

  err = esp_ota_set_boot_partition(s_ota_part);
  if (err != ESP_OK) {
    snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_set_boot: %s", esp_err_to_name(err));
    set_status("error", 0);
    s_busy = false;
    log_add("error", "OTA web gagal set boot: %s", g_ota_error);
    return false;
  }

  set_status("success", 100);
  g_ota_error[0] = 0;
  s_busy = false;
  log_add("info", "OTA web sukses, reboot dalam 3 detik");
  schedule_reboot(3000);
  return true;
}

void ota_web_abort(void) {
  if (s_sha_active) {
    mbedtls_sha256_free(&s_sha);
    s_sha_active = false;
  }
  if (s_web_active) {
    esp_ota_abort(s_ota_handle);
    s_ota_handle = 0;
    s_web_active = false;
  }
  if (s_busy) {
    set_status("error", 0);
    snprintf(g_ota_error, sizeof(g_ota_error), "Upload dibatalkan");
    s_busy = false;
  }
}

static void cloud_ota_task(void *arg) {
  (void)arg;

  char url[sizeof(g_config.fw_url)];
  char expected_sha[sizeof(g_config.fw_sha)];
  uint32_t expected_size = 0;

  config_lock(0);
  snprintf(url, sizeof(url), "%s", g_config.fw_url);
  snprintf(expected_sha, sizeof(expected_sha), "%s", g_config.fw_sha);
  expected_size = g_config.fw_size;
  config_unlock();

  set_status("downloading", 0);
  g_ota_error[0] = 0;
  log_add("warn", "OTA cloud mulai: %s", url);

  esp_http_client_config_t cfg = {
    .url = url,
    .timeout_ms = 15000,
    .keep_alive_enable = false,
  };
#if ALLOW_INSECURE_TLS
  cfg.skip_cert_common_name_check = true;
#else
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak bisa membuat HTTP client");
    set_status("error", 0);
    s_busy = false;
    vTaskDelete(NULL);
    return;
  }

  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak bisa membuka URL firmware");
    set_status("error", 0);
    esp_http_client_cleanup(client);
    s_busy = false;
    vTaskDelete(NULL);
    return;
  }

  int64_t content_length = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    snprintf(g_ota_error, sizeof(g_ota_error), "HTTP %d", status);
    set_status("error", 0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_busy = false;
    log_add("error", "OTA cloud gagal download (HTTP %d)", status);
    vTaskDelete(NULL);
    return;
  }

  size_t total = content_length > 0 ? (size_t)content_length : (size_t)expected_size;
  if (expected_size > 0 && content_length > 0 && (uint32_t)content_length != expected_size) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Ukuran firmware tidak cocok (%d != %lu)",
             (int)content_length, (unsigned long)expected_size);
    set_status("error", 0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_busy = false;
    log_add("error", "OTA cloud: %s", g_ota_error);
    vTaskDelete(NULL);
    return;
  }

  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (!part) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak ada partisi OTA tersedia");
    set_status("error", 0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_busy = false;
    vTaskDelete(NULL);
    return;
  }

  esp_ota_handle_t handle = 0;
  err = esp_ota_begin(part, total ? total : OTA_SIZE_UNKNOWN, &handle);
  if (err != ESP_OK) {
    snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_begin: %s", esp_err_to_name(err));
    set_status("error", 0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_busy = false;
    vTaskDelete(NULL);
    return;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  uint8_t buf[1024];
  size_t written = 0;
  bool failed = false;
  int read;
  while ((read = esp_http_client_read(client, (char *)buf, sizeof(buf))) > 0) {
    err = esp_ota_write(handle, buf, (size_t)read);
    if (err != ESP_OK) {
      snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_write: %s", esp_err_to_name(err));
      failed = true;
      break;
    }
    mbedtls_sha256_update(&sha, buf, (size_t)read);
    written += (size_t)read;
    if (total) g_ota_progress = (int)((written * 100ULL) / total);
  }

  if (!failed && total && written != total) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Download tidak lengkap (%u/%u)",
             (unsigned)written, (unsigned)total);
    failed = true;
  }
  if (!failed && expected_size > 0 && written != expected_size) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Ukuran firmware tidak cocok (%u != %lu)",
             (unsigned)written, (unsigned long)expected_size);
    failed = true;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (!failed && expected_sha[0] && is_hex64(expected_sha)) {
    char actual[65];
    sha_to_hex(digest, actual);
    if (strcasecmp(actual, expected_sha) != 0) {
      snprintf(g_ota_error, sizeof(g_ota_error), "SHA-256 firmware tidak cocok");
      failed = true;
    }
  }

  if (!failed) {
    set_status("verifying", g_ota_progress);
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
      snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_end: %s", esp_err_to_name(err));
      failed = true;
    } else {
      err = esp_ota_set_boot_partition(part);
      if (err != ESP_OK) {
        snprintf(g_ota_error, sizeof(g_ota_error), "esp_ota_set_boot: %s", esp_err_to_name(err));
        failed = true;
      }
    }
  } else {
    esp_ota_abort(handle);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (failed) {
    set_status("error", 0);
    log_add("error", "OTA cloud gagal: %s", g_ota_error);
  } else {
    set_status("success", 100);
    g_ota_error[0] = 0;
    log_add("info", "OTA cloud sukses, reboot dalam 3 detik");
    schedule_reboot(3000);
  }

  s_busy = false;
  vTaskDelete(NULL);
}

bool ota_cloud_start(void) {
  if (s_busy) {
    snprintf(g_ota_error, sizeof(g_ota_error), "OTA lain sedang berjalan");
    return false;
  }
  if (strlen(g_config.fw_url) == 0) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak ada firmware cloud tersedia");
    return false;
  }

  s_busy = true;
  set_status("starting", 0);
  g_ota_error[0] = 0;
  BaseType_t ok = xTaskCreate(cloud_ota_task, "cloud-ota", 12288, NULL, 4, NULL);
  if (ok != pdPASS) {
    set_status("error", 0);
    snprintf(g_ota_error, sizeof(g_ota_error), "Gagal membuat task OTA");
    s_busy = false;
    return false;
  }
  return true;
}
