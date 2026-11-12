#pragma once

#include <stdint.h>

void alarm_init(void);
void alarm_loop(void);
void alarm_acknowledge(void);
uint8_t alarm_level(void);
uint32_t alarm_active_mask(void);
