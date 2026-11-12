#pragma once

#include <Arduino.h>
#include "app_config.h"

enum LockPolicy : uint8_t {
  LOCK_PROVISION_ONLY = 0,
  LOCK_GRACE = 1,
  LOCK_STRICT = 2,
};

enum OfflineAction : uint8_t {
  OFFLINE_KEEP_LAST = 0,
  OFFLINE_FALLBACK_DEFAULT = 1,
  OFFLINE_LOCK_UI = 2,
};

enum AlarmLevel : uint8_t {
  ALARM_NORMAL = 0,
  ALARM_WARNING = 1,
  ALARM_ALARM = 2,
  ALARM_FAULT = 3,
};

struct Threshold {
  float warn_low;
  float alarm_low;
  float warn_high;
  float alarm_high;
  float hysteresis;
  char unit[8];
};

struct DeviceConfig {
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

  uint32_t quota_expires;   // epoch seconds, 0 = no expiry
  uint32_t last_sync;       // epoch seconds
  char plan[16];

  char fw_version[16];
  char fw_available_version[16];
  char fw_url[192];
  char fw_sha[65];
  uint32_t fw_size;
};

struct SensorState {
  float primary;
  float secondary;
  bool valid;
  uint8_t error_count;
  uint32_t last_read_ms;
  uint32_t last_read_epoch;
  uint8_t state;           // AlarmLevel
  uint8_t candidate_state;
  uint8_t candidate_count;
};

struct AlarmState {
  uint8_t state;           // AlarmLevel
  bool latched;
  bool acknowledged;
  uint32_t ack_time;
  uint32_t since;
  uint32_t active_mask;
  char summary[160];
};

struct CloudState {
  bool online;
  bool ever_synced;
  uint32_t last_sync;
  uint32_t last_attempt;
  char last_error[96];
};

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
const char* lockPolicyName(uint8_t policy);
const char* offlineActionName(uint8_t action);
const char* alarmLevelName(uint8_t level);
uint32_t nowEpoch();
bool timeIsValid();

// Config access is shared between the web server, the alarm engine and the
// background cloud/OTA workers. Use the recursive lock for any read or write
// that touches more than one field of g_config.
void stateBegin();
bool configLock(uint32_t timeoutMs = 100);
void configUnlock();

// SHA-256 helpers (hex output, 64 chars + NUL).
void sha256HexString(const char* input, char* out, size_t outLen);
void passwordHash(const char* plain, char* out, size_t outLen);
bool timeReached(uint32_t deadlineMs);
void scheduleReboot(uint32_t delayMs);
