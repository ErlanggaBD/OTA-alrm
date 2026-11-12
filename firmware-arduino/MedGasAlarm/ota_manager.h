#pragma once

#include <Arduino.h>

void otaBegin();
void otaLoop();
bool otaBusy();

// Web (raw body) OTA upload. Called from the WebServer callbacks.
bool otaWebStart(size_t total, const String& expectedSha);
bool otaWebWrite(const uint8_t* data, size_t len);
bool otaWebFinish();
void otaWebAbort();

// Cloud OTA download. Runs in a background task and reboots on success.
bool otaCloudStart();
