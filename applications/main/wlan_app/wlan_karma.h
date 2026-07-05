#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef void (*WlanKarmaCredCb)(const char* ssid, const uint8_t* client_mac, void* ctx);

bool wlan_karma_start(uint8_t channel, WlanKarmaCredCb cred_cb, void* ctx);
void wlan_karma_stop(void);
bool wlan_karma_is_running(void);

uint32_t wlan_karma_get_probe_reqs(void);
uint32_t wlan_karma_get_probe_resps(void);
uint16_t wlan_karma_get_ssid_count(void);
uint16_t wlan_karma_get_clients(void);
bool wlan_karma_get_current_ssid(char* out, size_t out_size);
bool wlan_karma_get_latest_probe_ssid(char* out, size_t out_size);
