#pragma once

#include <Arduino.h>

void alarmBegin();
void alarmLoop();
void alarmAcknowledge();
uint8_t alarmLevel();
uint32_t alarmActiveMask();
