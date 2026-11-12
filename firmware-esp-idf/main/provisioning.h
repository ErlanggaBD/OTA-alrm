#pragma once

#include <stdbool.h>
#include <stddef.h>

void provisioning_init(void);
void provisioning_start_ap(bool recovery);
void provisioning_stop_ap(void);
void provisioning_loop(void);
bool provisioning_is_recovery(void);

bool provisioning_provision(const char *email, const char *ssid, const char *pass,
                             const char *admin_password, const char *api_base,
                             char *error, size_t error_len,
                             char *message, size_t message_len);
