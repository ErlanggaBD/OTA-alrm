#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Build-time defaults. Override with -D flags / menuconfig if needed.
// ---------------------------------------------------------------------------
#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

#ifndef HW_REV
#define HW_REV "esp32-devkit-4mb"
#endif

#ifndef DEFAULT_API_BASE
#define DEFAULT_API_BASE "http://192.168.1.10:3000"
#endif

// Demo default: accept any TLS certificate. Set to 0 in production and fill
// API_ROOT_CA / use the ESP-IDF certificate bundle.
#ifndef ALLOW_INSECURE_TLS
#define ALLOW_INSECURE_TLS 1
#endif

// ---------------------------------------------------------------------------
// Hardware (8x DHT22 example; replace the sensor driver for real gas probes)
// ---------------------------------------------------------------------------
#define SENSOR_COUNT 8
extern const int SENSOR_PINS[SENSOR_COUNT];

#define BUZZER_PIN 4
#define RELAY_PIN 5
#define STATUS_LED_PIN 2
#define RESET_BUTTON_PIN 0

#define BUZZER_ACTIVE_HIGH 1
#define RELAY_ACTIVE_LOW 1
#define LED_ACTIVE_HIGH 1

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define SENSOR_READ_INTERVAL_MS 2000UL
#define ALARM_EVAL_INTERVAL_MS 500UL
#define CLOUD_SYNC_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)
#define CLOUD_RETRY_INTERVAL_MS (60UL * 1000UL)
#define WIFI_CONNECT_TIMEOUT_MS 20000UL
#define WIFI_RECONNECT_INTERVAL_MS 30000UL
#define NTP_WAIT_MS 6000UL
#define RESET_HOLD_MS 8000UL
#define RE_ALERT_MS (5UL * 60UL * 1000UL)
#define SESSION_TTL_MS (30UL * 60UL * 1000UL)
#define MAX_SESSIONS 4

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
#define CONFIG_MAGIC 0x4D474153UL  // "MGAS"
#define CONFIG_VERSION 1
#define CONFIG_NAMESPACE "mg_cfg"

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
#define DEFAULT_ACTIVE_SENSORS 4
#define DEFAULT_DEFAULT_ACTIVE_SENSORS 4
#define DEFAULT_GRACE_DAYS 7
#define DEFAULT_ADMIN_PASS "admin12345"
#define DEFAULT_OTA_PASS "medgasota"
#define DEFAULT_LOCK_POLICY 0  // provision_only
#define DEFAULT_OFFLINE_ACTION 0  // keep_last
#define DEFAULT_ALLOW_LOCAL_OVERRIDE 1
#define DEFAULT_AUTO_OTA 0

#define AP_PREFIX "MedGasAlarm-"
#define AP_PASS_PREFIX "medgas-"

// ---------------------------------------------------------------------------
// Alarm / sensor defaults
// ---------------------------------------------------------------------------
#define DEFAULT_WARN_LOW 5.0f
#define DEFAULT_ALARM_LOW 2.0f
#define DEFAULT_WARN_HIGH 35.0f
#define DEFAULT_ALARM_HIGH 40.0f
#define DEFAULT_HYSTERESIS 0.5f
#define DEFAULT_UNIT "C"
#define SENSOR_FAULT_AFTER_ERRORS 3
#define ALARM_DEBOUNCE_SAMPLES 2

// HTTP server stack. OTA + JSON handlers run here.
#define WEB_TASK_STACK 12288
