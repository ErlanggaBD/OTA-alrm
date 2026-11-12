#include "app_state.h"

#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

static SemaphoreHandle_t s_configMutex = nullptr;

const char* lockPolicyName(uint8_t policy) {
  switch (policy) {
    case LOCK_GRACE: return "grace";
    case LOCK_STRICT: return "strict";
    default: return "provision_only";
  }
}

const char* offlineActionName(uint8_t action) {
  switch (action) {
    case OFFLINE_FALLBACK_DEFAULT: return "fallback_default";
    case OFFLINE_LOCK_UI: return "lock_ui";
    default: return "keep_last";
  }
}

const char* alarmLevelName(uint8_t level) {
  switch (level) {
    case ALARM_WARNING: return "warning";
    case ALARM_ALARM: return "alarm";
    case ALARM_FAULT: return "fault";
    default: return "normal";
  }
}

uint32_t nowEpoch() {
  time_t now = time(nullptr);
  if (now < 1600000000) return 0;
  return (uint32_t)now;
}

bool timeIsValid() {
  return nowEpoch() != 0;
}

void stateBegin() {
  if (s_configMutex == nullptr) {
    s_configMutex = xSemaphoreCreateRecursiveMutex();
  }
}

bool configLock(uint32_t timeoutMs) {
  if (s_configMutex == nullptr) return true;
  TickType_t ticks = (timeoutMs == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  return xSemaphoreTakeRecursive(s_configMutex, ticks) == pdTRUE;
}

void configUnlock() {
  if (s_configMutex != nullptr) xSemaphoreGiveRecursive(s_configMutex);
}

void sha256HexString(const char* input, char* out, size_t outLen) {
  if (!out || outLen < 65) {
    if (out && outLen) out[0] = 0;
    return;
  }
  uint8_t hash[32];
  const unsigned char* data = (const unsigned char*)(input ? input : "");
  mbedtls_sha256(data, strlen((const char*)data), hash, 0);
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[(hash[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[hash[i] & 0x0F];
  }
  out[64] = 0;
}

void passwordHash(const char* plain, char* out, size_t outLen) {
  sha256HexString(plain ? plain : "", out, outLen);
}

bool timeReached(uint32_t deadlineMs) {
  return deadlineMs != 0 && (int32_t)(millis() - deadlineMs) >= 0;
}

void scheduleReboot(uint32_t delayMs) {
  g_reboot_at = millis() + delayMs;
  if (g_reboot_at == 0) g_reboot_at = 1;
}
