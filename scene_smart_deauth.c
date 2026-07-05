#include "../wlan_app.h"
#include "../wlan_hal.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SmartDeauthScene"

// External references
extern bool deauth_worker_start(WlanApp* app);
extern void deauth_worker_stop(void);
extern uint8_t deauth_get_most_active_channels(uint8_t* channels, uint8_t max_count);

void wlan_app_scene_smart_deauth_on_enter(void* context) {
    WlanApp* app = context;

    app->deauth_smart = true;
    app->channel_mode_active = true;

    // Use the smart deauth view
    WlanSmartDeauthModel* m = view_get_model(app->view_smart_deauther);
    memset(m, 0, sizeof(WlanSmartDeauthModel));

    m->scanning = true;
    m->running = false;
    m->frames_sent = 0;

    // Initial scan
    uint8_t channels[3] = {0};
    uint8_t count = deauth_get_most_active_channels(channels, 3);
    memcpy(m->active_channels, channels, sizeof(channels));
    m->active_count = count;

    view_commit_model(app->view_smart_deauther, true);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSmartDeauther);

    ESP_LOGI(TAG, "Smart Deauth GUI started - %d active channels", count);
}

bool wlan_app_scene_smart_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        if (event.event == WlanAppCustomEventDeautherStart || 
            event.event == WlanAppCustomEventDeautherAuto) {

            if (!app->deauth_auto) {
                if (deauth_worker_start(app)) {
                    app->deauth_auto = true;
                    ESP_LOGI(TAG, "Smart attack started");
                }
            } else {
                deauth_worker_stop();
                app->deauth_auto = false;
            }
            consumed = true;
        } 
        else if (event.event == WlanAppCustomEventDeautherStop) {
            deauth_worker_stop();
            app->deauth_auto = false;
            consumed = true;
        }
    } 
    else if (event.type == SceneManagerEventTypeTick) {
        WlanSmartDeauthModel* m = view_get_model(app->view_smart_deauther);
        
        m->frames_sent = s_frames_sent;
        m->running = s_task_run;

        // Update active channels periodically
        if (s_task_run && (s_frames_sent % 50 == 0)) {
            uint8_t channels[3] = {0};
            m->active_count = deauth_get_most_active_channels(channels, 3);
            memcpy(m->active_channels, channels, sizeof(channels));
        }

        view_commit_model(app->view_smart_deauther, true);
        consumed = true;
    }

    return consumed;
}

void wlan_app_scene_smart_deauth_on_exit(void* context) {
    WlanApp* app = context;
    deauth_worker_stop();
    app->deauth_auto = false;
}