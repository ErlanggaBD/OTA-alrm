#include "log_buffer.h"
#include "app_state.h"

#include <stdarg.h>
#include <time.h>

static LogEntry s_logs[LOG_BUFFER_SIZE];
static size_t s_head = 0;
static size_t s_count = 0;

static void makeTimestamp(char* out, size_t len) {
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    strftime(out, len, "%H:%M:%S", &tmNow);
  } else {
    snprintf(out, len, "T+%lus", (unsigned long)(millis() / 1000));
  }
}

void logBegin() {
  s_head = 0;
  s_count = 0;
  logAdd("info", "log buffer siap");
}

void logAdd(const char* level, const char* fmt, ...) {
  LogEntry* entry = &s_logs[s_head];
  makeTimestamp(entry->time, sizeof(entry->time));
  snprintf(entry->level, sizeof(entry->level), "%s", level ? level : "info");

  va_list args;
  va_start(args, fmt);
  vsnprintf(entry->message, sizeof(entry->message), fmt, args);
  va_end(args);

  s_head = (s_head + 1) % LOG_BUFFER_SIZE;
  if (s_count < LOG_BUFFER_SIZE) s_count++;

  Serial.printf("[%s] %-5s %s\n", entry->time, entry->level, entry->message);
}

size_t logSnapshot(LogEntry* out, size_t maxEntries) {
  if (!out || maxEntries == 0) return 0;
  size_t n = s_count < maxEntries ? s_count : maxEntries;
  for (size_t i = 0; i < n; i++) {
    size_t idx = (s_head + LOG_BUFFER_SIZE - 1 - i) % LOG_BUFFER_SIZE;
    out[i] = s_logs[idx];
  }
  return n;
}
