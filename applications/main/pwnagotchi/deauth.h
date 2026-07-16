#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define DEAUTH_MAX_CLIENTS 16
void deauth_list(void);
void deauth_add(const char* bssid);
bool deauth_select(void);
void deauth_run(void);
void deauth_get_target(char* out_essid, size_t essid_sz, uint8_t out_bssid[6]);
void deauth_collect_start(const uint8_t* bssid);
void deauth_collect_stop(void);
bool deauth_is_collecting(void);
void deauth_report_mac(const uint8_t* mac);
int deauth_get_client_count(void);
void deauth_get_ap_counts(int* now, int* total);
const uint8_t* deauth_get_client_mac(int index);