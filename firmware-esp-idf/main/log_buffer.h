#pragma once

#include <stddef.h>

#define LOG_BUFFER_SIZE 80
#define LOG_MESSAGE_SIZE 180

typedef struct {
  char time[20];
  char level[8];
  char message[LOG_MESSAGE_SIZE];
} LogEntry;

void log_init(void);
void log_add(const char *level, const char *fmt, ...);
size_t log_snapshot(LogEntry *out, size_t max_entries);
