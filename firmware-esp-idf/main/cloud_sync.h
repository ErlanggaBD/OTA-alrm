#pragma once

#include <stdbool.h>

#include "cJSON.h"

void cloud_init(void);
void cloud_loop(void);
bool cloud_request_sync(void);
bool cloud_sync_busy(void);
bool cloud_provision(const char *api_base, const char *email, const char *serial,
                     char **response_out);
bool cloud_apply_config(cJSON *config, bool save);
void cloud_apply_policy(void);
