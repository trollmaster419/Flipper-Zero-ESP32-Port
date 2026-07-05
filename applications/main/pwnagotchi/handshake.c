#include "handshake.h"
#include "config.h"
#include <furi.h>
#include <storage/storage.h>
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include <esp_attr.h>
#define TAG "Handshake"
#define HANDSHAVES_DIR "/ext/apps_data/pwnagotchi/handshakes"
struct pcapng_shb {
    uint32_t block_type;
    uint32_t block_total_length;
    uint32_t byte_order_magic;
    uint16_t major_version;
    uint16_t minor_version;
    int64_t section_length;
    uint32_t block_total_length2;
} __attribute__((packed));
struct pcapng_idb {
    uint32_t block_type;
    uint32_t block_total_length;
    uint16_t link_type;
    uint16_t reserved;
    uint32_t snaplen;
    uint32_t block_total_length2;
} __attribute__((packed));
struct pcapng_epb_header {
    uint32_t block_type;
    uint32_t block_total_length;
    uint32_t interface_id;
    uint32_t timestamp_high;
    uint32_t timestamp_low;
    uint32_t captured_pkt_len;
    uint32_t original_pkt_len;
} __attribute__((packed));
static HandshakeFrame* s_buffer = NULL;
static volatile uint32_t s_write_idx = 0;
static volatile uint32_t s_handshake_count = 0;
static bool s_capturing = false;
void IRAM_ATTR handshake_buffer_eapol(const uint8_t* frame, uint16_t len,
                            const uint8_t* bssid, uint32_t ts_usec) {
    if (!s_capturing || !s_buffer) return;
    if (!frame || len == 0) return;
    uint32_t idx = __sync_fetch_and_add(&s_write_idx, 1);
    if (idx >= HANDSHAKE_MAX_FRAMES) return;
    uint16_t copy_len = (len > 512) ? 512 : len;
    memcpy(s_buffer[idx].data, frame, copy_len);
    s_buffer[idx].len = copy_len;
    memcpy(s_buffer[idx].bssid, bssid, HANDSHAKE_BSSID_LEN);
    s_buffer[idx].ts_usec = ts_usec;
}
static void sanitize_essid(const char* src, char* dst, size_t dst_sz) {
    size_t i = 0;
    while (*src && i < dst_sz - 1) {
        char c = *src++;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_') {
            dst[i++] = c;
        } else if (c == ' ') {
            dst[i++] = '_';
        }
    }
    dst[i] = '\0';
}
static void write_pcapng(File* file, HandshakeFrame* frames, uint32_t count) {
    struct pcapng_shb shb = {
        .block_type = 0x0A0D0D0A,
        .block_total_length = sizeof(struct pcapng_shb),
        .byte_order_magic = 0x1A2B3C4D,
        .major_version = 1,
        .minor_version = 0,
        .section_length = -1,
        .block_total_length2 = sizeof(struct pcapng_shb)
    };
    storage_file_write(file, &shb, sizeof(shb));
    struct pcapng_idb idb = {
        .block_type = 1,
        .block_total_length = sizeof(struct pcapng_idb),
        .link_type = 105,
        .reserved = 0,
        .snaplen = 65535,
        .block_total_length2 = sizeof(struct pcapng_idb)
    };
    storage_file_write(file, &idb, sizeof(idb));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t padded_len = (frames[i].len + 3) & ~3;
        uint32_t block_len = sizeof(struct pcapng_epb_header) + padded_len + sizeof(uint32_t);
        struct pcapng_epb_header epb = {
            .block_type = 6,
            .block_total_length = block_len,
            .interface_id = 0,
            .timestamp_high = 0,
            .timestamp_low = 0,
            .captured_pkt_len = frames[i].len,
            .original_pkt_len = frames[i].len
        };
        uint64_t ts_usec = (uint64_t)frames[i].ts_usec;
        epb.timestamp_high = (uint32_t)(ts_usec >> 32);
        epb.timestamp_low = (uint32_t)(ts_usec & 0xFFFFFFFF);
        storage_file_write(file, &epb, sizeof(epb));
        storage_file_write(file, frames[i].data, frames[i].len);
        if (padded_len > frames[i].len) {
            uint32_t pad = 0;
            storage_file_write(file, &pad, padded_len - frames[i].len);
        }
        storage_file_write(file, &block_len, sizeof(block_len));
    }
}
uint32_t handshake_process(const char* ap_essid, const uint8_t* ap_bssid) {
    s_capturing = false;
    uint32_t total = s_write_idx;
    if (!s_buffer) total = 0;
    if (total > HANDSHAKE_MAX_FRAMES) total = HANDSHAKE_MAX_FRAMES;
    ESP_LOGI(TAG, "Processing %lu buffered EAPOL frames", (unsigned long)total);
    if (total == 0) {
        s_write_idx = 0;
        ESP_LOGI(TAG, "No EAPOL frames captured (target may have no clients)");
        return 0;
    }
    HandshakeFrame* temp = malloc(sizeof(HandshakeFrame) * HANDSHAKE_MAX_FRAMES);
    if (!temp) {
        ESP_LOGE(TAG, "Failed to allocate memory for temporary handshake frames");
        s_write_idx = 0;
        return 0;
    }
    uint32_t match_count = 0;
    if (ap_essid && ap_bssid) {
        size_t essid_len = strlen(ap_essid);
        if (essid_len > 32) essid_len = 32;
        memset(&temp[0], 0, sizeof(HandshakeFrame));
        memcpy(temp[0].bssid, ap_bssid, HANDSHAKE_BSSID_LEN);
        temp[0].ts_usec = 1000; 
        uint8_t* b = temp[0].data;
        b[0] = 0x80; b[1] = 0x00; 
        b[2] = 0x00; b[3] = 0x00; 
        memset(b+4, 0xFF, 6);     
        memcpy(b+10, ap_bssid, 6); 
        memcpy(b+16, ap_bssid, 6); 
        b[22] = 0x00; b[23] = 0x00; 
        memset(b+24, 0, 8); 
        b[32] = 0x64; b[33] = 0x00; 
        b[34] = 0x11; b[35] = 0x04; 
        b[36] = 0x00; 
        b[37] = essid_len; 
        memcpy(b+38, ap_essid, essid_len);
        uint32_t frame_len = 38 + essid_len;
        uint8_t rsn_ie[] = {
            0x30, 0x14, 
            0x01, 0x00, 
            0x00, 0x0f, 0xac, 0x04, 
            0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 
            0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 
            0x00, 0x00
        };
        memcpy(b + frame_len, rsn_ie, sizeof(rsn_ie));
        frame_len += sizeof(rsn_ie);
        memset(b+frame_len, 0, 4); 
        temp[0].len = frame_len + 4;
        match_count = 1;
    }
    uint8_t seen_bssids[HANDSHAKE_MAX_FRAMES][HANDSHAKE_BSSID_LEN];
    uint32_t seen_count = 0;
    for (uint32_t i = 0; i < total; i++) {
        HandshakeFrame* f = &s_buffer[i];
        if (ap_bssid) {
            bool match = (memcmp(f->bssid, ap_bssid, HANDSHAKE_BSSID_LEN) == 0);
            if (!match) continue;
        }
        temp[match_count++] = *f;
        bool found = false;
        for (uint32_t s = 0; s < seen_count; s++) {
            if (memcmp(seen_bssids[s], f->bssid, HANDSHAKE_BSSID_LEN) == 0) {
                found = true;
                break;
            }
        }
        if (!found && seen_count < HANDSHAKE_MAX_FRAMES) {
            memcpy(seen_bssids[seen_count], f->bssid, HANDSHAKE_BSSID_LEN);
            seen_count++;
        }
    }
    if (match_count <= 1) { 
        ESP_LOGW(TAG, "No frames matched BSSID (got %lu, all from other APs)", (unsigned long)total);
        s_write_idx = 0;
        free(temp);
        return 0;
    }
    bool has_m1 = false, has_m2 = false, has_m3 = false, has_m4 = false;
    for (uint32_t t = 0; t < match_count; t++) {
        uint8_t* pt = temp[t].data;
        if (pt[0] == 0x80) continue; 
        int hdr_len = (((pt[0] >> 4) & 0x0f) == 8) ? 26 : 24;
        if (temp[t].len < hdr_len + 8 + 99) continue;
        int eo = hdr_len + 8;
        if (pt[eo + 1] != 3) continue; 
        uint16_t kinfo = (pt[eo + 5] << 8) | pt[eo + 6];
        bool mic = (kinfo & 0x0100) != 0;
        bool ack = (kinfo & 0x0080) != 0;
        uint16_t key_data_len = (pt[eo + 97] << 8) | pt[eo + 98];
        if (ack && !mic) has_m1 = true;
        else if (!ack && mic) {
            if (key_data_len > 0) has_m2 = true;
            else has_m4 = true;
        }
        else if (ack && mic) has_m3 = true;
    }
    if (!has_m1 || !has_m2 || !has_m3 || !has_m4) {
        ESP_LOGW(TAG, "Incomplete handshake (1:%d 2:%d 3:%d 4:%d). Discarding.", has_m1, has_m2, has_m3, has_m4);
        s_write_idx = 0;
        free(temp);
        return 0xFFFFFFFF; 
    }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, "/ext/apps_data/pwnagotchi");
    storage_simply_mkdir(storage, HANDSHAVES_DIR);
    char essid_safe[32];
    sanitize_essid(ap_essid ? ap_essid : "unknown", essid_safe, sizeof(essid_safe));
    uint32_t now = esp_log_timestamp() / 1000;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s_%lu.pcapng", HANDSHAVES_DIR, essid_safe, (unsigned long)now);
    File* file = storage_file_alloc(storage);
    if (storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        write_pcapng(file, temp, match_count);
        storage_file_close(file);
        ESP_LOGI(TAG, "Saved %lu EAPOL frames to %s", (unsigned long)match_count, path);
    } else {
        ESP_LOGE(TAG, "Failed to create %s", path);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    s_handshake_count += seen_count;
    s_write_idx = 0;
    free(temp);
    return seen_count;
}
void handshake_start_capture(void) {
    if (!s_buffer) {
        s_buffer = malloc(sizeof(HandshakeFrame) * HANDSHAKE_MAX_FRAMES);
        if (!s_buffer) {
            ESP_LOGE(TAG, "Failed to alloc s_buffer");
            return;
        }
    }
    s_write_idx = 0;
    s_capturing = true;
}
void handshake_stop_capture(void) {
    s_capturing = false;
}
void handshake_reset_count(void) {
    s_handshake_count = 0;
}
uint32_t handshake_get_count(void) {
    return s_handshake_count;
}
void handshake_deinit(void) {
    s_capturing = false;
    if (s_buffer) {
        free(s_buffer);
        s_buffer = NULL;
    }
}