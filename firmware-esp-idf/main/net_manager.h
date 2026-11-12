#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

void net_init(void);
bool net_connect_to(const char *ssid, const char *pass, uint32_t timeout_ms);
bool net_connect_sta(uint32_t timeout_ms);
void net_loop(void);
bool net_is_connected(void);
int net_rssi(void);
const char *net_ip(void);
const char *net_ssid(void);
void net_sync_time(void);
bool net_check_internet(void);
