#include "ota_manager.h"
#include "app_config.h"
#include "app_state.h"
#include "log_buffer.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static volatile bool s_otaBusy = false;
static volatile bool s_webAttempted = false;
static volatile bool s_webUploadActive = false;
static volatile size_t s_webTotal = 0;
static volatile size_t s_webWritten = 0;
static bool s_autoAttempted = false;

static bool isHex64(const String& value) {
  if (value.length() != 64) return false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

static void setStatus(const char* status, int progress) {
  snprintf(g_ota_status, sizeof(g_ota_status), "%s", status ? status : "idle");
  g_ota_progress = progress;
}

bool otaBusy() {
  return s_otaBusy;
}

void otaBegin() {
  setStatus("idle", 0);
  g_ota_error[0] = 0;

  char hostname[32];
  snprintf(hostname, sizeof(hostname), "medgas-%s", g_serial);
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(g_config.ota_pass);
  ArduinoOTA.setRebootOnSuccess(true);
  ArduinoOTA.onStart([]() {
    s_otaBusy = true;
    setStatus("uploading", 0);
    g_ota_error[0] = 0;
    logAdd("warn", "OTA Arduino IDE dimulai");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total) g_ota_progress = (int)((progress * 100ULL) / total);
  });
  ArduinoOTA.onEnd([]() {
    setStatus("success", 100);
    logAdd("info", "OTA Arduino IDE selesai, reboot");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    setStatus("error", 0);
    snprintf(g_ota_error, sizeof(g_ota_error), "ArduinoOTA error %u", (unsigned)error);
    s_otaBusy = false;
    logAdd("error", "OTA Arduino IDE gagal: %u", (unsigned)error);
  });
  ArduinoOTA.begin();
  logAdd("info", "OTA Arduino IDE aktif (host %s)", hostname);
}

void otaLoop() {
  if (s_otaBusy) return;
  ArduinoOTA.handle();

  // Automatic cloud OTA: once per boot, only when the vendor enabled it.
  if (!s_autoAttempted && g_config.auto_ota && g_provisioned && !g_ap_mode &&
      WiFi.status() == WL_CONNECTED && strlen(g_config.fw_url) > 0) {
    s_autoAttempted = true;
    logAdd("info", "auto OTA ke %s", g_config.fw_available_version);
    otaCloudStart();
  }
}

bool otaWebStart(size_t total, const String& expectedSha) {
  if (s_otaBusy) {
    snprintf(g_ota_error, sizeof(g_ota_error), "OTA lain sedang berjalan");
    return false;
  }
  s_webAttempted = true;
  if (total == 0) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Ukuran firmware tidak valid");
    return false;
  }

  s_otaBusy = true;
  s_webUploadActive = false;
  s_webTotal = total;
  s_webWritten = 0;
  setStatus("uploading", 0);
  g_ota_error[0] = 0;

  if (!Update.begin(total, U_FLASH)) {
    snprintf(g_ota_error, sizeof(g_ota_error), "%s", Update.errorString());
    setStatus("error", 0);
    s_otaBusy = false;
    logAdd("error", "OTA web gagal mulai: %s", g_ota_error);
    return false;
  }

  if (expectedSha.length()) {
    if (!isHex64(expectedSha)) {
      snprintf(g_ota_error, sizeof(g_ota_error), "SHA-256 header tidak valid");
      setStatus("error", 0);
      Update.abort();
      s_otaBusy = false;
      return false;
    }
    if (!Update.setSHA256(expectedSha.c_str())) {
      snprintf(g_ota_error, sizeof(g_ota_error), "Tidak bisa memasang verifikasi SHA-256");
      setStatus("error", 0);
      Update.abort();
      s_otaBusy = false;
      return false;
    }
  }

  s_webUploadActive = true;
  logAdd("warn", "OTA web dimulai (%u byte%s)", (unsigned)total, expectedSha.length() ? ", SHA-256" : "");
  return true;
}

bool otaWebWrite(const uint8_t* data, size_t len) {
  if (!s_webUploadActive || !data || len == 0) return false;
  size_t written = Update.write((uint8_t*)data, len);
  if (written != len) {
    snprintf(g_ota_error, sizeof(g_ota_error), "%s", Update.errorString());
    setStatus("error", 0);
    Update.abort();
    s_webUploadActive = false;
    s_otaBusy = false;
    logAdd("error", "OTA web gagal menulis: %s", g_ota_error);
    return false;
  }
  s_webWritten += written;
  if (s_webTotal) {
    g_ota_progress = (int)((s_webWritten * 100ULL) / s_webTotal);
  }
  return true;
}

bool otaWebFinish() {
  if (!s_webAttempted) {
    return false;
  }
  s_webAttempted = false;

  if (!s_webUploadActive) {
    if (g_ota_error[0] == 0) {
      snprintf(g_ota_error, sizeof(g_ota_error), "Upload firmware tidak aktif");
    }
    s_otaBusy = false;
    return false;
  }

  s_webUploadActive = false;
  setStatus("verifying", g_ota_progress);
  bool ok = Update.end();
  if (!ok) {
    snprintf(g_ota_error, sizeof(g_ota_error), "%s", Update.errorString());
    setStatus("error", 0);
    s_otaBusy = false;
    logAdd("error", "OTA web gagal verifikasi: %s", g_ota_error);
    return false;
  }

  setStatus("success", 100);
  g_ota_error[0] = 0;
  s_otaBusy = false;
  logAdd("info", "OTA web sukses, reboot dalam 3 detik");
  scheduleReboot(3000);
  return true;
}

void otaWebAbort() {
  if (!s_webAttempted) return;
  s_webAttempted = false;
  if (s_webUploadActive) {
    Update.abort();
    s_webUploadActive = false;
  }
  if (s_otaBusy) {
    setStatus("error", 0);
    snprintf(g_ota_error, sizeof(g_ota_error), "Upload dibatalkan");
    s_otaBusy = false;
  }
}

static void cloudOtaTask(void*) {
  char url[sizeof(g_config.fw_url)];
  char sha[sizeof(g_config.fw_sha)];
  uint32_t expectedSize = 0;

  configLock(portMAX_DELAY);
  strncpy(url, g_config.fw_url, sizeof(url) - 1);
  url[sizeof(url) - 1] = 0;
  strncpy(sha, g_config.fw_sha, sizeof(sha) - 1);
  sha[sizeof(sha) - 1] = 0;
  expectedSize = g_config.fw_size;
  configUnlock();

  setStatus("downloading", 0);
  g_ota_error[0] = 0;
  logAdd("warn", "OTA cloud mulai: %s", url);

  WiFiClient plain;
  WiFiClientSecure secure;
  NetworkClient* client;
  if (String(url).startsWith("https://")) {
#if ALLOW_INSECURE_TLS
    secure.setInsecure();
#else
    if (strlen(API_ROOT_CA) > 0) secure.setCACert(API_ROOT_CA);
    else secure.setInsecure();
#endif
    client = &secure;
  } else {
    client = &plain;
  }

  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(*client, String(url))) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak bisa membuka URL firmware");
    setStatus("error", 0);
    logAdd("error", "OTA cloud: %s", g_ota_error);
    s_otaBusy = false;
    vTaskDelete(nullptr);
    return;
  }

  int code = http.GET();
  if (code != 200) {
    snprintf(g_ota_error, sizeof(g_ota_error), "HTTP %d", code);
    setStatus("error", 0);
    logAdd("error", "OTA cloud gagal download (HTTP %d)", code);
    http.end();
    s_otaBusy = false;
    vTaskDelete(nullptr);
    return;
  }

  int len = http.getSize();
  if (len <= 0) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Ukuran firmware tidak diketahui");
    setStatus("error", 0);
    logAdd("error", "OTA cloud: %s", g_ota_error);
    http.end();
    s_otaBusy = false;
    vTaskDelete(nullptr);
    return;
  }
  if (expectedSize > 0 && (uint32_t)len != expectedSize) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Ukuran firmware tidak cocok (%d != %lu)", len, (unsigned long)expectedSize);
    setStatus("error", 0);
    logAdd("error", "OTA cloud: %s", g_ota_error);
    http.end();
    s_otaBusy = false;
    vTaskDelete(nullptr);
    return;
  }

  if (!Update.begin((size_t)len, U_FLASH)) {
    snprintf(g_ota_error, sizeof(g_ota_error), "%s", Update.errorString());
    setStatus("error", 0);
    logAdd("error", "OTA cloud gagal mulai: %s", g_ota_error);
    http.end();
    s_otaBusy = false;
    vTaskDelete(nullptr);
    return;
  }

  if (strlen(sha) == 64) {
    if (!Update.setSHA256(sha)) {
      snprintf(g_ota_error, sizeof(g_ota_error), "Tidak bisa memasang verifikasi SHA-256");
      setStatus("error", 0);
      Update.abort();
      http.end();
      s_otaBusy = false;
      vTaskDelete(nullptr);
      return;
    }
  }

  NetworkClient& stream = http.getStream();
  uint8_t buf[1024];
  size_t written = 0;
  uint32_t lastData = millis();
  bool failed = false;

  while (written < (size_t)len) {
    size_t avail = stream.available();
    if (avail == 0) {
      if (!stream.connected()) break;
      if (millis() - lastData > 15000UL) {
        snprintf(g_ota_error, sizeof(g_ota_error), "Download timeout");
        failed = true;
        break;
      }
      delay(5);
      continue;
    }

    size_t n = stream.readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n == 0) continue;
    if (Update.write(buf, n) != n) {
      snprintf(g_ota_error, sizeof(g_ota_error), "%s", Update.errorString());
      failed = true;
      break;
    }
    written += n;
    lastData = millis();
    g_ota_progress = (int)((written * 100ULL) / (size_t)len);
  }

  if (!failed && written != (size_t)len) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Download tidak lengkap (%u/%d)", (unsigned)written, len);
    failed = true;
  }

  if (!failed) {
    setStatus("verifying", g_ota_progress);
    if (!Update.end()) {
      snprintf(g_ota_error, sizeof(g_ota_error), "%s", Update.errorString());
      failed = true;
    }
  }

  http.end();

  if (failed) {
    Update.abort();
    setStatus("error", 0);
    logAdd("error", "OTA cloud gagal: %s", g_ota_error);
  } else {
    setStatus("success", 100);
    g_ota_error[0] = 0;
    logAdd("info", "OTA cloud sukses, reboot dalam 3 detik");
    scheduleReboot(3000);
  }

  s_otaBusy = false;
  vTaskDelete(nullptr);
}

bool otaCloudStart() {
  if (s_otaBusy) {
    snprintf(g_ota_error, sizeof(g_ota_error), "OTA lain sedang berjalan");
    return false;
  }
  if (strlen(g_config.fw_url) == 0) {
    snprintf(g_ota_error, sizeof(g_ota_error), "Tidak ada firmware cloud tersedia");
    return false;
  }

  s_otaBusy = true;
  setStatus("starting", 0);
  g_ota_error[0] = 0;
  BaseType_t ok = xTaskCreate(cloudOtaTask, "cloud-ota", 12288, nullptr, 1, nullptr);
  if (ok != pdPASS) {
    setStatus("error", 0);
    snprintf(g_ota_error, sizeof(g_ota_error), "Gagal membuat task OTA");
    s_otaBusy = false;
    return false;
  }
  return true;
}
