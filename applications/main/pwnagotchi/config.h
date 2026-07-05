#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <esp_wifi.h>
#define PWNAGOTCHI_WHITELIST_MAX 8
#define PWNAGOTCHI_CHANNELS_COUNT 13
typedef struct {
    bool deauth;
    bool advertise;
    bool scan;
    bool spam;
    char ssid[32];
    char pass[64];
    int short_delay_ms;
    int long_delay_ms;
    int baud;
    int init_channel;
    char whitelist[PWNAGOTCHI_WHITELIST_MAX][32];
    int whitelist_count;
    char happy[8];
    char sad[8];
    char broken[8];
    char intense[8];
    char looking1[8];
    char looking2[8];
    char neutral[8];
    char sleeping[8];
    int epoch;
    char face[8];
    char identity[65];
    char name[32];
    int ap_ttl;
    bool associate;
    int bored_num_epochs;
    int channels[PWNAGOTCHI_CHANNELS_COUNT];
    int excited_num_epochs;
    int hop_recon_time;
    int max_inactive_scale;
    int max_interactions;
    int max_misses_for_recon;
    int min_recon_time;
    int min_rssi;
    int recon_inactive_multiplier;
    int recon_time;
    int sad_num_epochs;
    int sta_ttl;
    int pwnd_run;
    int pwnd_tot;
    char session_id[18];
    int uptime;
    char version[16];
    wifi_country_t country;
} PwnagotchiConfig;
extern PwnagotchiConfig g_config;
extern bool g_config_loaded;
void config_init(void);
void config_load(void);
void pwn_config_save(void);
int config_random(int min, int max);
int config_uptime_secs(void);