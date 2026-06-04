#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t active_channels[3];
    uint8_t active_count;
    uint32_t frames_sent;
    bool running;
    uint8_t ap_counts[14];
    bool auto_mode;
} WlanSmartDeauthModel;

View* wlan_smart_deauth_view_alloc(void);
void wlan_smart_deauth_view_free(View* view);