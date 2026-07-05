#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>
typedef struct {
    bool detected;
    int rssi;
    int channel;
    char bssid[18];
    char name[64];
    char pwnd_tot[16];
    char device_type[16]; 
    bool is_minigotchi;
    bool is_pal;
} PwnagotchiPeer;
extern volatile bool g_pwnagotchi_detected;
extern PwnagotchiPeer g_last_peer;
void pwnagotchi_detect_start(void);
void pwnagotchi_detect_stop(void);
void pwnagotchi_detect(void);
void pwnagotchi_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type);
void handshake_reset_count(void);
uint32_t handshake_get_count(void);