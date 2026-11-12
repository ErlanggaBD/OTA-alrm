#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ota_init(void);
void ota_loop(void);
bool ota_busy(void);

// Web (raw body) OTA upload. Called from the HTTP server handler.
bool ota_web_begin(size_t total, const char *expected_sha);
bool ota_web_write(const uint8_t *data, size_t len);
bool ota_web_finish(void);
void ota_web_abort(void);

// Cloud OTA download. Runs in a background task and reboots on success.
bool ota_cloud_start(void);
