#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

void cloudBegin();
void cloudLoop();
bool cloudRequestSync();
bool cloudSyncBusy();
bool cloudProvision(const String& apiBase, const String& email, const String& serial, String& responseOut);
bool cloudApplyConfig(JsonObjectConst config, bool save);
void cloudApplyPolicy();
