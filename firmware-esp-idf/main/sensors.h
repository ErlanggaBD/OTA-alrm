#pragma once

#include <stdint.h>

void sensors_init(void);
void sensors_loop(void);
uint32_t sensor_age_seconds(uint8_t idx);
