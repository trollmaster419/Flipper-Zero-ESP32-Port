#include "pwnagotchi_detect.h"
#include "handshake.h"
#include "deauth.h"
#include "config.h"
#include "../wlan_app/wlan_hal.h"
#include <furi.h>
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#define TAG "PwnDetect"
#define PWNAGOTCHI_MAC "de:ad:be:ef:de:ad"
volatile bool g_pwnagotchi_detected = false;
PwnagotchiPeer g_last_peer;
typedef struct __attribute__((packed)) {
    int16_t fctl;
    int16_t duration;
    uint8_t da[6];
    uint8_t sa[6];
    uint8_t bssid[6];
    int16_t seqctl;
    unsigned char payload[];
} WifiMgmtHdr;
typedef struct __attribute__((packed)) {
    WifiMgmtHdr hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;
static void get_mac(char* addr, const unsigned char* buff, int offset) {
    snprintf(addr, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             buff[offset], buff[offset + 1], buff[offset + 2],
             buff[offset + 3], buff[offset + 4], buff[offset + 5]);
}
static char* extract_json_str(const char* json, const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char* key_pos = strstr(json, search);
    if (!key_pos) return NULL;
    char* val_pos = key_pos + strlen(search);
    while (*val_pos && (*val_pos == ' ' || *val_pos == ':' || *val_pos == '\t')) val_pos++;
    if (!*val_pos) return NULL;
    if (*val_pos == '"') {
        val_pos++;
        char* end = strchr(val_pos, '"');
        if (!end) return NULL;
        size_t len = end - val_pos;
        if (len > 63) len = 63;
        char* result = malloc(len + 1);
        if (result) {
            strncpy(result, val_pos, len);
            result[len] = '\0';
        }
        return result;
    }
    return NULL;
}
static bool extract_json_bool(const char* json, const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char* key_pos = strstr(json, search);
    if (!key_pos) return false;
    char* val_pos = key_pos + strlen(search);
    while (*val_pos && (*val_pos == ' ' || *val_pos == ':' || *val_pos == '\t')) val_pos++;
    return (strncmp(val_pos, "true", 4) == 0);
}
static char* extract_json_int_str(const char* json, const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char* key_pos = strstr(json, search);
    if (!key_pos) return NULL;
    char* val_pos = key_pos + strlen(search);
    while (*val_pos && (*val_pos == ' ' || *val_pos == ':' || *val_pos == '\t')) val_pos++;
    if (!*val_pos || !isdigit((unsigned char)*val_pos)) return NULL;
    char* end = val_pos;
    while (isdigit((unsigned char)*end)) end++;
    size_t len = end - val_pos;
    if (len > 15) len = 15;
    char* result = malloc(len + 1);
    if (result) {
        strncpy(result, val_pos, len);
        result[len] = '\0';
    }
    return result;
}
void pwnagotchi_detect_start(void) {
    g_pwnagotchi_detected = false;
    memset(&g_last_peer, 0, sizeof(g_last_peer));
}
void pwnagotchi_detect_stop(void) {
    g_pwnagotchi_detected = false;
}
void pwnagotchi_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MGMT) {
        wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
        if (pkt->payload[0] != 0x80) return;
        char src[18];
        get_mac(src, pkt->payload, 10);
        if (strcmp(src, PWNAGOTCHI_MAC) != 0) return;
        g_pwnagotchi_detected = true;
        g_last_peer.detected = true;
        g_last_peer.rssi = pkt->rx_ctrl.rssi;
        g_last_peer.channel = pkt->rx_ctrl.channel;
        strncpy(g_last_peer.bssid, src, sizeof(g_last_peer.bssid));
        int len = pkt->rx_ctrl.sig_len;
        char essid[512] = {0};
        int essid_pos = 0;
        for (int i = 38; i < len && i < (int)sizeof(pkt->payload) && essid_pos < (int)sizeof(essid) - 1; i++) {
            char c = (char)pkt->payload[i];
            if (isprint((unsigned char)c) || c == '\n' || c == '\t') {
                essid[essid_pos++] = c;
            }
        }
        essid[essid_pos] = '\0';
        if (strlen(essid) < 5 || essid[0] != '{') return;
        char* name = extract_json_str(essid, "name");
        char* pwnd_tot = extract_json_int_str(essid, "pwnd_tot");
        bool is_minigotchi = extract_json_bool(essid, "minigotchi");
        bool is_pal = extract_json_bool(essid, "pal");
        if (name) {
            strncpy(g_last_peer.name, name, sizeof(g_last_peer.name) - 1);
            free(name);
        } else {
            strncpy(g_last_peer.name, "N/A", sizeof(g_last_peer.name));
        }
        if (pwnd_tot) {
            strncpy(g_last_peer.pwnd_tot, pwnd_tot, sizeof(g_last_peer.pwnd_tot) - 1);
            free(pwnd_tot);
        } else {
            strncpy(g_last_peer.pwnd_tot, "N/A", sizeof(g_last_peer.pwnd_tot));
        }
        g_last_peer.is_minigotchi = is_minigotchi;
        g_last_peer.is_pal = is_pal;
        if (is_minigotchi) {
            strncpy(g_last_peer.device_type, "Minigotchi", sizeof(g_last_peer.device_type));
        } else if (is_pal) {
            strncpy(g_last_peer.device_type, "Palnagotchi", sizeof(g_last_peer.device_type));
        } else {
            strncpy(g_last_peer.device_type, "Pwnagotchi", sizeof(g_last_peer.device_type));
        }
        ESP_LOGI(TAG, "%s name: %s, pwnd_tot: %s",
                 g_last_peer.device_type, g_last_peer.name, g_last_peer.pwnd_tot);
        return;
    }
    if (type != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t fc0 = pkt->payload[0];
    if ((fc0 & 0x0C) != 0x08) return;
    uint8_t subtype = fc0 >> 4;
    if (subtype == 4 || subtype == 0x0C) return;
    deauth_report_mac(pkt->payload + 4);   
    deauth_report_mac(pkt->payload + 10);  
    int hdr_len = 24;
    if (subtype == 8) hdr_len = 26;
    int sig_len = pkt->rx_ctrl.sig_len;
    int llc_off = hdr_len;
    if (sig_len < llc_off + 8) return;
    if (pkt->payload[llc_off + 0] == 0xAA &&
        pkt->payload[llc_off + 1] == 0xAA &&
        pkt->payload[llc_off + 2] == 0x03 &&
        pkt->payload[llc_off + 6] == 0x88 &&
        pkt->payload[llc_off + 7] == 0x8E) {
        uint8_t bssid[6];
        uint8_t fc1 = pkt->payload[1];
        if (fc1 & 0x01) {
            memcpy(bssid, pkt->payload + 4, 6);
        } else {
            memcpy(bssid, pkt->payload + 10, 6);
        }
        handshake_buffer_eapol(pkt->payload, sig_len, bssid,
                               pkt->rx_ctrl.timestamp);
    }
}
void pwnagotchi_detect(void) {
    if (!g_config.scan) return;
    pwnagotchi_detect_start();
    furi_delay_ms(g_config.long_delay_ms);
    pwnagotchi_detect_stop();
    if (g_pwnagotchi_detected) {
        ESP_LOGI(TAG, "Pwnagotchi detected!");
    } else {
        ESP_LOGI(TAG, "No Pwnagotchi found");
    }
}