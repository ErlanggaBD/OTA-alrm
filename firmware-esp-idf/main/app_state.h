#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

typedef enum {
  LOCK_PROVISION_ONLY = 0,
  LOCK_GRACE = 1,
  LOCK_STRICT = 2,
} LockPolicy;

typedef enum {
  OFFLINE_KEEP_LAST = 0,
  OFFLINE_FALLBACK_DEFAULT = 1,
  OFFLINE_LOCK_UI = 2,
} OfflineAction;

typedef enum {
  ALARM_NORMAL = 0,
  ALARM_WARNING = 1,
  ALARM_ALARM = 2,
  ALARM_FAULT = 3,
} AlarmLevel;

typedef struct {
  float warn_low;
  float alarm_low;
  float warn_high;
  float alarm_high;
  float hysteresis;
  char unit[8];
} Threshold;

typedef struct {
  uint32_t magic;
  uint16_t version;
  char email[96];
  char ssid[33];
  char wifi_pass[65];
  char admin_pass[65];
  char device_secret[65];
  char serial[24];
  char api_base[128];
  char ota_pass[33];

  uint8_t active_sensors;
  uint8_t lock_policy;
  uint8_t offline_action;
  uint8_t default_active_sensors;
  uint16_t grace_days;
  bool allow_local_override;
  bool auto_ota;

  char labels[SENSOR_COUNT][24];
  Threshold thresholds[SENSOR_COUNT];

  uint32_t quota_expires;  // epoch seconds, 0 = no expiry
  uint32_t last_sync;      // epoch seconds
  char plan[16];

  char fw_version[16];
  char fw_available_version[16];
  char fw_url[192];
  char fw_sha[65];
  uint32_t fw_size;
} DeviceConfig;

typedef struct {
  float primary;
  float secondary;
  bool valid;
  uint8_t error_count;
  uint32_t last_read_ms;
  uint32_t last_read_epoch;
  uint8_t state;            // AlarmLevel
  uint8_t candidate_state;
  uint8_t candidate_count;
} SensorState;

typedef struct {
  uint8_t state;            // AlarmLevel
  bool latched;
  bool acknowledged;
  uint32_t ack_time;
  uint32_t since;
  uint32_t active_mask;
  char summary[160];
} AlarmState;

typedef struct {
  bool online;
  bool ever_synced;
  uint32_t last_sync;
  uint32_t last_attempt;
  char last_error[96];
} CloudState;

extern DeviceConfig g_config;
extern SensorState g_sensors[SENSOR_COUNT];
extern AlarmState g_alarm;
extern CloudState g_cloud;

extern bool g_provisioned;
extern bool g_ap_mode;
extern bool g_ui_locked;
extern char g_lock_reason[128];
extern char g_serial[24];
extern char g_ap_ssid[32];
extern char g_ap_pass[32];

extern char g_ota_status[20];
extern int g_ota_progress;
extern char g_ota_error[96];
extern uint32_t g_reboot_at;

// Shared helpers
uint32_t millis_now(void);
uint32_t now_epoch(void);
bool time_valid(void);
bool time_reached(uint32_t deadline_ms);
void schedule_reboot(uint32_t delay_ms);

const char *lock_policy_name(uint8_t policy);
const char *offline_action_name(uint8_t action);
const char *alarm_level_name(uint8_t level);

// Config access is shared between the web server, alarm engine and background
// cloud/OTA workers. Pass 0 to block forever, otherwise a timeout in ms.
void state_init(void);
bool config_lock(uint32_t timeout_ms);
void config_unlock(void);

// SHA-256 helpers (hex output, 64 chars + NUL).
void sha256_hex_string(const char *input, char *out, size_t out_len);
void password_hash(const char *plain, char *out, size_t out_len);
