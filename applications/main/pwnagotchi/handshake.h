#pragma once
#include <stdint.h>
#include <stdbool.h>
#define HANDSHAKE_MAX_FRAMES 64
#define HANDSHAKE_BSSID_LEN 6
typedef struct {
    uint8_t data[512];
    uint16_t len;
    uint8_t bssid[HANDSHAKE_BSSID_LEN];
    uint32_t ts_usec;
} HandshakeFrame;
void handshake_buffer_eapol(const uint8_t* frame, uint16_t len,
                            const uint8_t* bssid, uint32_t ts_usec);
uint32_t handshake_process(const char* ap_essid, const uint8_t* ap_bssid);
void handshake_start_capture(void);
void handshake_stop_capture(void);
void handshake_reset_count(void);
uint32_t handshake_get_count(void);
void handshake_deinit(void);