#pragma once

#include <Arduino.h>

#define LOG_BUFFER_SIZE 80
#define LOG_MESSAGE_SIZE 180

struct LogEntry {
  char time[20];
  char level[8];
  char message[LOG_MESSAGE_SIZE];
};

void logBegin();
void logAdd(const char* level, const char* fmt, ...);
size_t logSnapshot(LogEntry* out, size_t maxEntries);
