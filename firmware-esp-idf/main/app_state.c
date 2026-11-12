#include "app_state.h"

#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"

static SemaphoreHandle_t s_config_mutex = NULL;

uint32_t millis_now(void) {
  return (uint32_t)(esp_timer_get_time() / 1000LL);
}

uint32_t now_epoch(void) {
  time_t now = time(NULL);
  if (now < 1600000000) return 0;
  return (uint32_t)now;
}

bool time_valid(void) {
  return now_epoch() != 0;
}

bool time_reached(uint32_t deadline_ms) {
  return deadline_ms != 0 && (int32_t)(millis_now() - deadline_ms) >= 0;
}

void schedule_reboot(uint32_t delay_ms) {
  g_reboot_at = millis_now() + delay_ms;
  if (g_reboot_at == 0) g_reboot_at = 1;
}

const char *lock_policy_name(uint8_t policy) {
  switch (policy) {
    case LOCK_GRACE: return "grace";
    case LOCK_STRICT: return "strict";
    default: return "provision_only";
  }
}

const char *offline_action_name(uint8_t action) {
  switch (action) {
    case OFFLINE_FALLBACK_DEFAULT: return "fallback_default";
    case OFFLINE_LOCK_UI: return "lock_ui";
    default: return "keep_last";
  }
}

const char *alarm_level_name(uint8_t level) {
  switch (level) {
    case ALARM_WARNING: return "warning";
    case ALARM_ALARM: return "alarm";
    case ALARM_FAULT: return "fault";
    default: return "normal";
  }
}

void state_init(void) {
  if (s_config_mutex == NULL) {
    s_config_mutex = xSemaphoreCreateRecursiveMutex();
  }
}

bool config_lock(uint32_t timeout_ms) {
  if (s_config_mutex == NULL) return true;
  TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(s_config_mutex, ticks) == pdTRUE;
}

void config_unlock(void) {
  if (s_config_mutex != NULL) xSemaphoreGiveRecursive(s_config_mutex);
}

void sha256_hex_string(const char *input, char *out, size_t out_len) {
  if (!out || out_len < 65) {
    if (out && out_len) out[0] = 0;
    return;
  }
  uint8_t hash[32];
  const unsigned char *data = (const unsigned char *)(input ? input : "");
  mbedtls_sha256(data, strlen((const char *)data), hash, 0);
  static const char *hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[(hash[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[hash[i] & 0x0F];
  }
  out[64] = 0;
}

void password_hash(const char *plain, char *out, size_t out_len) {
  sha256_hex_string(plain ? plain : "", out, out_len);
}
