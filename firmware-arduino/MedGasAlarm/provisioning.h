#pragma once

#include <Arduino.h>

void provisioningBegin();
void provisioningStartAp(bool recovery);
void provisioningStopAp();
void provisioningLoop();
bool provisioningIsRecovery();
bool provisioningProvision(
  const String& email,
  const String& ssid,
  const String& pass,
  const String& adminPassword,
  const String& apiBase,
  String& errorOut,
  String& messageOut
);
