#include "log_buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "log";
static LogEntry s_logs[LOG_BUFFER_SIZE];
static size_t s_head = 0;
static size_t s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static void make_timestamp(char *out, size_t len) {
  if (time_valid()) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, len, "%H:%M:%S", &tm_now);
  } else {
    snprintf(out, len, "T+%lus", (unsigned long)(millis_now() / 1000UL));
  }
}

void log_init(void) {
  if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
  s_head = 0;
  s_count = 0;
  log_add("info", "log buffer siap");
}

void log_add(const char *level, const char *fmt, ...) {
  LogEntry entry;
  make_timestamp(entry.time, sizeof(entry.time));
  snprintf(entry.level, sizeof(entry.level), "%s", level ? level : "info");

  va_list args;
  va_start(args, fmt);
  vsnprintf(entry.message, sizeof(entry.message), fmt, args);
  va_end(args);

  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_logs[s_head] = entry;
  s_head = (s_head + 1) % LOG_BUFFER_SIZE;
  if (s_count < LOG_BUFFER_SIZE) s_count++;
  if (s_mutex) xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "[%s] %-5s %s", entry.time, entry.level, entry.message);
}

size_t log_snapshot(LogEntry *out, size_t max_entries) {
  if (!out || max_entries == 0) return 0;
  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
  size_t n = s_count < max_entries ? s_count : max_entries;
  for (size_t i = 0; i < n; i++) {
    size_t idx = (s_head + LOG_BUFFER_SIZE - 1 - i) % LOG_BUFFER_SIZE;
    out[i] = s_logs[idx];
  }
  if (s_mutex) xSemaphoreGive(s_mutex);
  return n;
}
