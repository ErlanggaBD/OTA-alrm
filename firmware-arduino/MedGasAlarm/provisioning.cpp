#include "provisioning.h"
#include "app_config.h"
#include "app_state.h"
#include "cloud_sync.h"
#include "log_buffer.h"
#include "net_manager.h"
#include "storage.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>

static DNSServer s_dns;
static bool s_recovery = false;

void provisioningBegin() {
  s_recovery = false;
}

void provisioningStartAp(bool recovery) {
  s_recovery = recovery;
  g_ap_mode = true;

  // Do not let the station auto-connect while the user is provisioning; the
  // provisioning handler controls each connection attempt explicitly.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);

  if (strlen(g_ap_ssid) == 0) {
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s%04X", AP_PREFIX, (unsigned)(esp_random() & 0xFFFF));
  }
  if (strlen(g_ap_pass) < 8) {
    snprintf(g_ap_pass, sizeof(g_ap_pass), "%s%04X", AP_PASS_PREFIX, (unsigned)(esp_random() & 0xFFFF));
  }

  bool ok = WiFi.softAP(g_ap_ssid, g_ap_pass);
  delay(200);
  if (!ok) {
    logAdd("error", "gagal menyalakan AP %s", g_ap_ssid);
    return;
  }

  s_dns.start(53, "*", WiFi.softAPIP());
  logAdd("info", "AP %s aktif, password %s, IP %s", g_ap_ssid, g_ap_pass, WiFi.softAPIP().toString().c_str());
  if (recovery) {
    logAdd("warn", "mode pemulihan: perbaiki Wi-Fi lewat captive portal");
  }
}

void provisioningStopAp() {
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  g_ap_mode = false;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
}

void provisioningLoop() {
  if (g_ap_mode) s_dns.processNextRequest();
}

bool provisioningIsRecovery() {
  return s_recovery;
}

static void returnToAp() {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  WiFi.setAutoReconnect(false);
}

bool provisioningProvision(
  const String& email,
  const String& ssid,
  const String& pass,
  const String& adminPassword,
  const String& apiBase,
  String& errorOut,
  String& messageOut
) {
  errorOut = "";
  messageOut = "";

  if (!g_ap_mode) {
    errorOut = "Alat tidak dalam mode registrasi";
    return false;
  }

  String targetEmail = email;
  targetEmail.trim();

  if (g_provisioned) {
    // Recovery mode: the identity is fixed, only Wi-Fi/server can change.
    targetEmail = String(g_config.email);
    char hash[65];
    passwordHash(adminPassword.c_str(), hash, sizeof(hash));
    if (strcmp(hash, g_config.admin_pass) != 0) {
      errorOut = "Password admin salah";
      return false;
    }
  } else {
    if (targetEmail.length() < 5 || targetEmail.indexOf('@') < 0 || targetEmail.length() > 95) {
      errorOut = "Email ID tidak valid";
      return false;
    }
    if (adminPassword.length() < 8) {
      errorOut = "Password admin minimal 8 karakter";
      return false;
    }
  }

  if (ssid.length() == 0 || ssid.length() > 32) {
    errorOut = "SSID tidak valid";
    return false;
  }

  String targetApi = apiBase;
  targetApi.trim();
  if (targetApi.length() == 0) targetApi = String(g_config.api_base);
  if (targetApi.length() == 0) targetApi = DEFAULT_API_BASE;
  if (!targetApi.startsWith("http://") && !targetApi.startsWith("https://")) {
    errorOut = "Alamat server harus diawali http:// atau https://";
    return false;
  }
  while (targetApi.endsWith("/")) targetApi.remove(targetApi.length() - 1);

  logAdd("info", "registrasi: email=%s ssid=%s server=%s", targetEmail.c_str(), ssid.c_str(), targetApi.c_str());

  if (!netConnectTo(ssid.c_str(), pass.c_str(), WIFI_CONNECT_TIMEOUT_MS)) {
    returnToAp();
    errorOut = "Gagal terhubung ke Wi-Fi. Periksa nama jaringan dan password.";
    return false;
  }

  String response;
  if (!cloudProvision(targetApi, targetEmail, String(g_config.serial), response)) {
    returnToAp();
    errorOut = "Tidak bisa menghubungi server vendor di " + targetApi + ". Periksa alamat server dan internet.";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    returnToAp();
    errorOut = "Respons server tidak valid";
    return false;
  }

  if (!(doc["ok"] | false)) {
    const char* serverError = doc["error"] | "Registrasi ditolak server";
    returnToAp();
    errorOut = String(serverError);
    return false;
  }

  const char* secret = doc["device_secret"] | "";
  if (strlen(secret) < 32) {
    returnToAp();
    errorOut = "Server tidak mengirim device secret";
    return false;
  }

  configLock(portMAX_DELAY);
  strncpy(g_config.email, targetEmail.c_str(), sizeof(g_config.email) - 1);
  g_config.email[sizeof(g_config.email) - 1] = 0;
  strncpy(g_config.ssid, ssid.c_str(), sizeof(g_config.ssid) - 1);
  g_config.ssid[sizeof(g_config.ssid) - 1] = 0;
  strncpy(g_config.wifi_pass, pass.c_str(), sizeof(g_config.wifi_pass) - 1);
  g_config.wifi_pass[sizeof(g_config.wifi_pass) - 1] = 0;
  strncpy(g_config.api_base, targetApi.c_str(), sizeof(g_config.api_base) - 1);
  g_config.api_base[sizeof(g_config.api_base) - 1] = 0;
  strncpy(g_config.device_secret, secret, sizeof(g_config.device_secret) - 1);
  g_config.device_secret[sizeof(g_config.device_secret) - 1] = 0;
  strncpy(g_config.serial, g_serial, sizeof(g_config.serial) - 1);
  g_config.serial[sizeof(g_config.serial) - 1] = 0;
  strncpy(g_config.fw_version, FW_VERSION, sizeof(g_config.fw_version) - 1);
  g_config.fw_version[sizeof(g_config.fw_version) - 1] = 0;
  passwordHash(adminPassword.c_str(), g_config.admin_pass, sizeof(g_config.admin_pass));
  configUnlock();

  // Apply the quota/config returned by the provisioning call.
  cloudApplyConfig(doc["config"].as<JsonObjectConst>(), false);

  configLock(portMAX_DELAY);
  storageSave();
  g_provisioned = true;
  configUnlock();

  g_cloud.online = true;
  g_cloud.ever_synced = true;
  g_cloud.last_sync = nowEpoch();
  g_cloud.last_error[0] = 0;

  WiFi.setAutoReconnect(true);
  messageOut = "Registrasi berhasil. Alat akan reboot ke dashboard.";
  logAdd("info", "registrasi selesai untuk %s", targetEmail.c_str());
  scheduleReboot(3000);
  return true;
}
