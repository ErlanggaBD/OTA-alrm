#pragma once

#include <stdbool.h>

void storage_init(void);
void storage_set_defaults(void);
bool storage_load(void);
bool storage_save(void);
void storage_clear(void);
