#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../views/wlan_smart_deauth_view.h"
#include <esp_log.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define TAG "SmartDeauth"

extern bool deauth_worker_start(WlanApp* app);
extern void deauth_worker_stop(void);
extern uint8_t deauth_get_most_active_channels(uint8_t* channels, uint8_t max_count);
extern uint32_t s_frames_sent;
extern bool s_task_run;
extern bool s_use_channel_mode;
extern uint8_t s_target_channel;

void wlan_app_scene_smart_deauth_on_enter(void* context) {
    WlanApp* app = (WlanApp*)context;
    app->deauth_smart = true;
    app->channel_mode_active = true;
    s_use_channel_mode = true;

    WlanSmartDeauthModel* m = view_get_model(app->view_smart_deauther);
    if (m) {
        memset(m, 0, sizeof(WlanSmartDeauthModel));

        uint8_t channels[3] = {0};
        m->active_count = deauth_get_most_active_channels(channels, 3);
        memcpy(m->active_channels, channels, sizeof(channels));
        if (m->active_count > 0) s_target_channel = m->active_channels[0];
    }

    view_commit_model(app->view_smart_deauther, true);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSmartDeauther);
}

bool wlan_app_scene_smart_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = (WlanApp*)context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        if (event.event == WlanAppCustomEventDeautherStart || 
            event.event == WlanAppCustomEventDeautherAuto) {
            
            if (!app->deauth_auto) {
                s_use_channel_mode = true;
                WlanSmartDeauthModel* m = view_get_model(app->view_smart_deauther);
                if (m && m->active_count > 0) s_target_channel = m->active_channels[0];
                view_commit_model(app->view_smart_deauther, false);
                deauth_worker_start(app);
                app->deauth_auto = true;
            } else {
                deauth_worker_stop();
                app->deauth_auto = false;
            }
            consumed = true;
        }
    } 
    else if (event.type == SceneManagerEventTypeTick) {
        WlanSmartDeauthModel* m = view_get_model(app->view_smart_deauther);
        if (m) {
            m->frames_sent = s_frames_sent;
            m->running = s_task_run;
            m->auto_mode = app->deauth_auto;

            static uint8_t scan_tick = 0;
            if (++scan_tick >= 8) {
                scan_tick = 0;
                uint8_t chs[3] = {0};
                m->active_count = deauth_get_most_active_channels(chs, 3);
                memcpy(m->active_channels, chs, sizeof(chs));

                memset(m->ap_counts, 0, 14);
                wifi_ap_record_t* recs = NULL;
                uint16_t count = 0;
                wlan_hal_scan(&recs, &count, 64);
                if (recs) {
                    for (uint16_t i = 0; i < count; i++) {
                        uint8_t ch = recs[i].primary;
                        if (ch > 0 && ch < 14) m->ap_counts[ch]++;
                    }
                    free(recs);
                }

                if (m->active_count > 0) {
                    static uint8_t ch_rot = 0;
                    ch_rot = (ch_rot + 1) % m->active_count;
                    s_target_channel = m->active_channels[ch_rot];
                    if (s_task_run) wlan_hal_set_channel(s_target_channel);
                }
            }

            view_commit_model(app->view_smart_deauther, false);
        }
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_smart_deauth_on_exit(void* context) {
    WlanApp* app = (WlanApp*)context;
    deauth_worker_stop();
    app->deauth_auto = false;
    app->deauth_smart = false;
}