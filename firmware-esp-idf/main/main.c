/*
 * MedGasAlarm - ESP-IDF build (ESP32 DevKit, 4 MB flash)
 *
 * Build:  idf.py set-target esp32 && idf.py build
 * Flash:  idf.py -p /dev/ttyUSB0 flash monitor
 *
 * The alarm engine (sensors + buzzer/relay) runs locally and never depends on
 * Wi-Fi, internet or the vendor backend.
 */

#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "app_config.h"
#include "app_state.h"
#include "cloud_sync.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "net_manager.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "provisioning.h"
#include "sensors.h"
#include "storage.h"
#include "web_server.h"

static const char *TAG = "main";

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
static bool s_button_handled = false;
static uint32_t s_button_down_at = 0;

static void reset_button_init(void) {
  gpio_config_t cfg = {
    .pin_bit_mask = 1ULL << RESET_BUTTON_PIN,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
}

static void reset_button_loop(void) {
  static int last_level = 1;
  int level = gpio_get_level(RESET_BUTTON_PIN);

  if (level == 0 && last_level == 1) {
    s_button_down_at = millis_now();
    s_button_handled = false;
  }

  if (level == 0 && !s_button_handled && s_button_down_at != 0 &&
      millis_now() - s_button_down_at >= RESET_HOLD_MS) {
    s_button_handled = true;
    log_add("warn", "tombol reset ditahan %lus, factory reset",
            (unsigned long)(RESET_HOLD_MS / 1000UL));
    storage_clear();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
  }

  if (level == 1) s_button_down_at = 0;
  last_level = level;
}

// ---------------------------------------------------------------------------
// Boot helpers
// ---------------------------------------------------------------------------
static void print_banner(void) {
  ESP_LOGI(TAG, "==============================================");
  ESP_LOGI(TAG, " MedGasAlarm - ESP32 medical gas alarm");
  ESP_LOGI(TAG, " firmware : %s", FW_VERSION);
  ESP_LOGI(TAG, " hardware : %s", HW_REV);
  ESP_LOGI(TAG, " serial   : %s", g_serial);
  ESP_LOGI(TAG, "==============================================");
}

static void enter_provisioning(bool recovery) {
  provisioning_start_ap(recovery);
  g_ui_locked = false;
  g_lock_reason[0] = 0;

  ESP_LOGI(TAG, "----------------------------------------------");
  ESP_LOGI(TAG, " AP SSID  : %s", g_ap_ssid);
  ESP_LOGI(TAG, " AP PASS  : %s", g_ap_pass);
  ESP_LOGI(TAG, " mode     : %s", recovery ? "recovery (Wi-Fi/server)" : "registrasi awal");
  ESP_LOGI(TAG, "----------------------------------------------");
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  state_init();
  storage_init();
  log_init();
  storage_load();

  print_banner();

  sensors_init();
  alarm_init();
  net_init();
  cloud_init();
  provisioning_init();

  if (!g_provisioned) {
    log_add("info", "belum terdaftar, membuka captive portal");
    enter_provisioning(false);
  } else {
    log_add("info", "terdaftar sebagai %s, mencoba konek Wi-Fi", g_config.email);
    if (net_connect_sta(WIFI_CONNECT_TIMEOUT_MS)) {
      g_ap_mode = false;
      g_ui_locked = false;
      g_lock_reason[0] = 0;
      log_add("info", "dashboard siap di http://%s/", net_ip());
    } else {
      log_add("warn", "Wi-Fi gagal, masuk mode pemulihan");
      enter_provisioning(true);
    }
  }

  web_init();
  ota_init();
  reset_button_init();

  cloud_apply_policy();
  if (g_provisioned && !g_ap_mode) {
    g_cloud.last_attempt = 0;
    cloud_request_sync();
  }

  // Give the first sensor reads a chance before the first alarm evaluation.
  sensors_loop();
  alarm_loop();

  for (;;) {
    if (g_reboot_at != 0 && time_reached(g_reboot_at)) {
      g_reboot_at = 0;
      log_add("info", "reboot terjadwal");
      vTaskDelay(pdMS_TO_TICKS(150));
      esp_restart();
    }

    reset_button_loop();
    provisioning_loop();
    web_loop();

    sensors_loop();
    alarm_loop();

    if (!g_ap_mode) {
      net_loop();
      cloud_loop();
      ota_loop();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
