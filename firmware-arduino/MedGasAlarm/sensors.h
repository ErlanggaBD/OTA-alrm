#pragma once

#include <Arduino.h>

void sensorsBegin();
void sensorsLoop();
uint32_t sensorAgeSeconds(uint8_t idx);
