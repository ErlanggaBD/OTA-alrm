#pragma once

#include <Arduino.h>

void storageBegin();
void storageSetDefaults();
bool storageLoad();
bool storageSave();
void storageClear();
