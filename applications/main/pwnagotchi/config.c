#include "config.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdlib.h>
#include <stdio.h>
#define TAG "PwnConfig"
PwnagotchiConfig g_config;
bool g_config_loaded = false;
void config_init(void) {
    g_config.deauth = true;
    g_config.advertise = true;
    g_config.scan = true;
    g_config.spam = true;
    strncpy(g_config.ssid, "minigotchi", sizeof(g_config.ssid));
    strncpy(g_config.pass, "dj1ch-minigotchi", sizeof(g_config.pass));
    g_config.short_delay_ms = 500;
    g_config.long_delay_ms = 5000;
    g_config.baud = 115200;
    g_config.init_channel = 1;
    g_config.whitelist_count = 0;
    strncpy(g_config.happy, "(^-^)", sizeof(g_config.happy));
    strncpy(g_config.sad, "(;-;)", sizeof(g_config.sad));
    strncpy(g_config.broken, "(X-X)", sizeof(g_config.broken));
    strncpy(g_config.intense, "(>-<)", sizeof(g_config.intense));
    strncpy(g_config.looking1, "(0-o)", sizeof(g_config.looking1));
    strncpy(g_config.looking2, "(o-0)", sizeof(g_config.looking2));
    strncpy(g_config.neutral, "('-')", sizeof(g_config.neutral));
    strncpy(g_config.sleeping, "(-.-)", sizeof(g_config.sleeping));
    g_config.epoch = 0;
    strncpy(g_config.face, "(^-^)", sizeof(g_config.face));
    strncpy(g_config.identity, "b9210077f7c14c0651aa338c55e820e93f90110ef679648001b1cecdbffc0090", sizeof(g_config.identity));
    strncpy(g_config.name, "minigotchi", sizeof(g_config.name));
    g_config.ap_ttl = config_random(30, 600);
    g_config.associate = true;
    g_config.bored_num_epochs = config_random(5, 30);
    int chs[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    memcpy(g_config.channels, chs, sizeof(chs));
    g_config.excited_num_epochs = config_random(5, 30);
    g_config.hop_recon_time = config_random(5, 60);
    g_config.max_inactive_scale = config_random(3, 10);
    g_config.max_interactions = config_random(1, 25);
    g_config.max_misses_for_recon = config_random(3, 10);
    g_config.min_recon_time = config_random(1, 30);
    g_config.min_rssi = config_random(-200, -50);
    g_config.recon_inactive_multiplier = config_random(1, 3);
    g_config.recon_time = config_random(5, 60);
    g_config.sad_num_epochs = config_random(5, 30);
    g_config.sta_ttl = config_random(60, 300);
    g_config.pwnd_run = 0;
    g_config.pwnd_tot = 0;
    strncpy(g_config.session_id, "84:f3:eb:58:95:bd", sizeof(g_config.session_id));
    g_config.uptime = config_uptime_secs();
    g_config.country = (wifi_country_t){.cc = "US", .schan = 1, .nchan = 13};
    strncpy(g_config.version, "3.6.4-beta", sizeof(g_config.version));
    g_config_loaded = false;
}
void config_load(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err != ESP_OK) return;
    uint8_t val = 0;
    if (nvs_get_u8(h, "configured", &val) == ESP_OK) {
        g_config_loaded = (val == 1);
    }
    size_t sz = 0;
    if (nvs_get_str(h, "whitelist", NULL, &sz) == ESP_OK && sz > 0) {
        char* buf = malloc(sz);
        if (buf) {
            if (nvs_get_str(h, "whitelist", buf, &sz) == ESP_OK) {
                g_config.whitelist_count = 0;
                char* token = strtok(buf, ",");
                while (token && g_config.whitelist_count < PWNAGOTCHI_WHITELIST_MAX) {
                    strncpy(g_config.whitelist[g_config.whitelist_count], token, 32);
                    g_config.whitelist_count++;
                    token = strtok(NULL, ",");
                }
            }
            free(buf);
        }
    }
    nvs_close(h);
}
void pwn_config_save(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) return;
    uint8_t val = g_config_loaded ? 1 : 0;
    nvs_set_u8(h, "configured", val);
    char buf[512] = {0};
    size_t pos = 0;
    for (int i = 0; i < g_config.whitelist_count && i < PWNAGOTCHI_WHITELIST_MAX; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s",
                        (pos > 0) ? "," : "", g_config.whitelist[i]);
    }
    nvs_set_str(h, "whitelist", buf);
    nvs_commit(h);
    nvs_close(h);
}
int config_random(int min, int max) {
    if (max <= min) return min;
    return min + (int)(esp_random() % (unsigned int)(max - min + 1));
}
int config_uptime_secs(void) {
    return (int)(esp_timer_get_time() / 1000000);
}