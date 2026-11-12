/*
 * MedGasAlarm - ESP32 medical gas / environmental alarm (Arduino IDE build)
 *
 * Target board : ESP32 Dev Module (4 MB flash)
 * Partition    : "Minimal SPIFFS (1.9MB APP x2)" -> min_spiffs
 *
 * Required libraries:
 *   - ArduinoJson            (>= 7.x)
 *   - DHT sensor library     (Adafruit, >= 1.4.x)
 *   - Adafruit Unified Sensor
 *
 * Flow:
 *   first boot / no identity  -> SoftAP + captive portal (setup.html)
 *   identity but no Wi-Fi     -> recovery captive portal (same page)
 *   provisioned + online      -> dashboard (index.html), cloud sync + OTA
 *
 * The alarm engine (sensors + buzzer/relay) always runs locally and never
 * depends on internet, Wi-Fi or the vendor backend.
 */

#include <Arduino.h>
#include <WiFi.h>

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
#include "web_server.h"

// ---------------------------------------------------------------------------
// Global state (declared extern in app_state.h)
// ---------------------------------------------------------------------------
DeviceConfig g_config;
SensorState g_sensors[SENSOR_COUNT];
AlarmState g_alarm;
CloudState g_cloud;

bool g_provisioned = false;
bool g_ap_mode = false;
bool g_ui_locked = false;
char g_lock_reason[128] = {0};
char g_serial[24] = {0};
char g_ap_ssid[32] = {0};
char g_ap_pass[32] = {0};

char g_ota_status[20] = "idle";
int g_ota_progress = 0;
char g_ota_error[96] = {0};
uint32_t g_reboot_at = 0;

// ---------------------------------------------------------------------------
// Reset button (GPIO0 / BOOT). Hold to factory reset.
// ---------------------------------------------------------------------------
static uint32_t s_buttonDownAt = 0;
static bool s_buttonHandled = false;

static void resetButtonLoop() {
  static bool lastLevel = HIGH;
  bool level = digitalRead(RESET_BUTTON_PIN);

  if (level == LOW && lastLevel == HIGH) {
    s_buttonDownAt = millis();
    s_buttonHandled = false;
  }

  if (level == LOW && !s_buttonHandled &&
      s_buttonDownAt != 0 && (millis() - s_buttonDownAt) >= RESET_HOLD_MS) {
    s_buttonHandled = true;
    logAdd("warn", "tombol reset ditahan %lus, factory reset", (unsigned long)(RESET_HOLD_MS / 1000UL));
    storageClear();
    delay(200);
    ESP.restart();
  }

  if (level == HIGH) {
    s_buttonDownAt = 0;
  }
  lastLevel = level;
}

// ---------------------------------------------------------------------------
// Boot helpers
// ---------------------------------------------------------------------------
static void printBanner() {
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" MedGasAlarm - ESP32 medical gas alarm");
  Serial.printf(" firmware : %s\n", FW_VERSION);
  Serial.printf(" hardware : %s\n", HW_REV);
  Serial.printf(" serial   : %s\n", g_serial);
  Serial.println("==============================================");
}

static void enterProvisioning(bool recovery) {
  provisioningStartAp(recovery);
  g_ui_locked = false;
  g_lock_reason[0] = 0;

  Serial.println();
  Serial.println("----------------------------------------------");
  Serial.printf(" AP SSID  : %s\n", g_ap_ssid);
  Serial.printf(" AP PASS  : %s\n", g_ap_pass);
  Serial.printf(" portal   : http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.printf(" mode     : %s\n", recovery ? "recovery (Wi-Fi/server)" : "registrasi awal");
  Serial.println("----------------------------------------------");
  Serial.println();
}

static void startCloudSyncSoon() {
  if (!g_provisioned || g_ap_mode) return;
  g_cloud.last_attempt = 0;
  cloudRequestSync();
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  stateBegin();
  storageBegin();
  logBegin();
  storageLoad();

  printBanner();

  sensorsBegin();
  alarmBegin();
  netBegin();
  cloudBegin();

  if (!g_provisioned) {
    logAdd("info", "belum terdaftar, membuka captive portal");
    enterProvisioning(false);
  } else {
    logAdd("info", "terdaftar sebagai %s, mencoba konek Wi-Fi", g_config.email);
    if (netConnectSta(WIFI_CONNECT_TIMEOUT_MS)) {
      g_ap_mode = false;
      g_ui_locked = false;
      g_lock_reason[0] = 0;
      logAdd("info", "dashboard siap di http://%s/", netIp().c_str());
    } else {
      logAdd("warn", "Wi-Fi gagal, masuk mode pemulihan");
      enterProvisioning(true);
    }
  }

  webBegin();
  otaBegin();

  cloudApplyPolicy();
  startCloudSyncSoon();

  // Give the first sensor reads a chance before the first alarm evaluation.
  sensorsLoop();
  alarmLoop();
}

void loop() {
  if (g_reboot_at != 0 && timeReached(g_reboot_at)) {
    g_reboot_at = 0;
    logAdd("info", "reboot terjadwal");
    delay(150);
    ESP.restart();
  }

  resetButtonLoop();
  provisioningLoop();
  webLoop();

  sensorsLoop();
  alarmLoop();

  if (!g_ap_mode) {
    netLoop();
    cloudLoop();
    otaLoop();
  }
}
