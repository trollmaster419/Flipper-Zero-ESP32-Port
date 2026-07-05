#include "frame.h"
#include "config.h"
#include "channel.h"
#include <furi.h>
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../wlan_app/wlan_hal.h"
#define TAG "PwnFrame"
#define ID_WHISPER_PAYLOAD     0xDE
#define ID_WHISPER_COMPRESSION 0xDF
#define ID_WHISPER_IDENTITY    0xE0
#define ID_WHISPER_SIGNATURE   0xE1
#define ID_WHISPER_STREAM_HDR  0xE2
static const uint8_t s_beacon_header[36] = {
    0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xde, 0xad, 0xbe, 0xef,
    0xde, 0xad, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x11, 0x04,
};
#define PWNGRIH_HDR_LEN sizeof(s_beacon_header)
#define CHUNK_SIZE 255
static char* build_json(bool include_minigotchi) {
    static char buf[2048];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{");
    if (include_minigotchi) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"minigotchi\":true,");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\"epoch\":%d,"
        "\"face\":\"%s\","
        "\"identity\":\"%s\","
        "\"name\":\"%s\","
        "\"policy\":{"
            "\"advertise\":%s,"
            "\"ap_ttl\":%d,"
            "\"associate\":%s,"
            "\"bored_num_epochs\":%d,"
            "\"deauth\":%s,"
            "\"excited_num_epochs\":%d,"
            "\"hop_recon_time\":%d,"
            "\"max_inactive_scale\":%d,"
            "\"max_interactions\":%d,"
            "\"max_misses_for_recon\":%d,"
            "\"min_recon_time\":%d,"
            "\"min_rssi\":%d,"
            "\"recon_inactive_multiplier\":%d,"
            "\"recon_time\":%d,"
            "\"sad_num_epochs\":%d,"
            "\"sta_ttl\":%d"
        "},"
        "\"pwnd_run\":%d,"
        "\"pwnd_tot\":%d,"
        "\"session_id\":\"%s\","
        "\"uptime\":%d,"
        "\"version\":\"%s\""
        "}",
        g_config.epoch,
        g_config.face,
        g_config.identity,
        g_config.name,
        g_config.advertise ? "true" : "false",
        g_config.ap_ttl,
        g_config.associate ? "true" : "false",
        g_config.bored_num_epochs,
        g_config.deauth ? "true" : "false",
        g_config.excited_num_epochs,
        g_config.hop_recon_time,
        g_config.max_inactive_scale,
        g_config.max_interactions,
        g_config.max_misses_for_recon,
        g_config.min_recon_time,
        g_config.min_rssi,
        g_config.recon_inactive_multiplier,
        g_config.recon_time,
        g_config.sad_num_epochs,
        g_config.sta_ttl,
        g_config.pwnd_run,
        g_config.pwnd_tot,
        g_config.session_id,
        g_config.uptime,
        g_config.version);
    if (pos >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "JSON truncated at %d bytes", pos);
    }
    return buf;
}
static uint8_t* build_beacon(const char* json, size_t* out_len) {
    // hello mister phone would you consent to redirecting your handshakes to me?
    // yes mr flipper i do
    size_t json_len = strlen(json);
    size_t hdr_len = 2 + ((json_len / 255) * 2); 
    size_t total = PWNGRIH_HDR_LEN + json_len + hdr_len;
    uint8_t* frame = malloc(total);
    if (!frame) return NULL;
    memcpy(frame, s_beacon_header, PWNGRIH_HDR_LEN);
    size_t frame_pos = PWNGRIH_HDR_LEN;
    for (size_t i = 0; i < json_len; i++) {
        if (i == 0 || i % 255 == 0) {
            frame[frame_pos++] = ID_WHISPER_PAYLOAD;
            uint8_t chunk_len = CHUNK_SIZE;
            if (json_len - i < CHUNK_SIZE) {
                chunk_len = (uint8_t)(json_len - i);
            }
            frame[frame_pos++] = chunk_len;
        }
        uint8_t c = (uint8_t)json[i];
        if (c < 32 || c > 126) c = '?';
        frame[frame_pos++] = c;
    }
    *out_len = frame_pos;
    return frame;
}
static bool send_frame(uint8_t* frame, size_t len) {
    // hello mister ap do you consent to be cracked?
    // yes mr flipper i do
    return wlan_hal_send_raw(frame, len);
}
bool frame_send(void) {
    bool ok = true;
    char* json = build_json(false);
    size_t len;
    uint8_t* frame = build_beacon(json, &len);
    if (frame) {
        ok = send_frame(frame, len);
        free(frame);
        if (!ok) return false;
    }
    json = build_json(true);
    frame = build_beacon(json, &len);
    if (frame) {
        ok = send_frame(frame, len);
        free(frame);
    }
    return ok;
}
void frame_advertise(void) {
    if (!g_config.advertise) return;
    ESP_LOGI(TAG, "Starting advertisement...");
    int packets = 0;
    int64_t start = esp_timer_get_time();
    for (int i = 0; i < 500; i++) {
        if (frame_send()) {
            packets++;
            float elapsed = (float)(esp_timer_get_time() - start) / 1000000.0f;
            if (elapsed > 0.001f && (i % 10 == 0)) {
                float pps = (float)packets / elapsed;
                ESP_LOGI(TAG, "PPS: %.1f pkt/s (CH: %d)", pps, channel_get());
            }
        } else {
            ESP_LOGW(TAG, "Advertisement failed to send!");
        }
        furi_delay_ms(10);
    }
    ESP_LOGI(TAG, "Advertisement finished! Sent %d packets", packets);
}