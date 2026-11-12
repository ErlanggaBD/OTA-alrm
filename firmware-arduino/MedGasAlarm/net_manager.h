#pragma once

#include <Arduino.h>

#include "app_config.h"

void netBegin();
bool netConnectTo(const char* ssid, const char* pass, uint32_t timeoutMs);
bool netConnectSta(uint32_t timeoutMs = WIFI_CONNECT_TIMEOUT_MS);
void netLoop();
bool netIsConnected();
int netRssi();
String netIp();
String netSsid();
void netSyncTime();
bool netCheckInternet();
