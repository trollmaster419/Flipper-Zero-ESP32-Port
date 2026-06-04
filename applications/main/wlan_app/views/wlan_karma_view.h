#pragma once

#include <gui/view.h>
#include <stdint.h>

typedef struct WlanKarmaView WlanKarmaView;

typedef void (*WlanKarmaViewActionCb)(void* ctx);

WlanKarmaView* wlan_karma_view_alloc(void);
void wlan_karma_view_free(WlanKarmaView* v);
View* wlan_karma_view_get_view(WlanKarmaView* v);

void wlan_karma_view_set_channel(WlanKarmaView* v, uint8_t channel);
void wlan_karma_view_set_probe_reqs(WlanKarmaView* v, uint32_t count);
void wlan_karma_view_set_probe_resps(WlanKarmaView* v, uint32_t count);
void wlan_karma_view_set_ssid_count(WlanKarmaView* v, uint16_t count);
void wlan_karma_view_set_clients(WlanKarmaView* v, uint16_t count);
void wlan_karma_view_set_running(WlanKarmaView* v, bool running);
void wlan_karma_view_set_current_ssid(WlanKarmaView* v, const char* ssid);
void wlan_karma_view_set_action_callback(WlanKarmaView* v, WlanKarmaViewActionCb cb, void* ctx);
