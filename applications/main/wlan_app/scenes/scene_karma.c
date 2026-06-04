#include "../wlan_app.h"
#include "../wlan_karma.h"

static void karma_action_cb(void* ctx) {
    WlanApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventKarmaToggle);
}

void wlan_app_scene_karma_on_enter(void* context) {
    WlanApp* app = context;

    wlan_karma_view_set_action_callback(app->karma_view_obj, karma_action_cb, app);
    wlan_karma_view_set_channel(app->karma_view_obj, 0);
    wlan_karma_view_set_running(app->karma_view_obj, false);
    wlan_karma_view_set_probe_reqs(app->karma_view_obj, 0);
    wlan_karma_view_set_probe_resps(app->karma_view_obj, 0);
    wlan_karma_view_set_ssid_count(app->karma_view_obj, 0);
    wlan_karma_view_set_clients(app->karma_view_obj, 0);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewKarma);

    // Auto-start with a default channel
    uint8_t ch = app->channel_action_channel ? app->channel_action_channel : 6;
    wlan_karma_view_set_channel(app->karma_view_obj, ch);
    if(!wlan_karma_start(ch, NULL, NULL)) {
        wlan_karma_view_set_running(app->karma_view_obj, false);
    } else {
        wlan_karma_view_set_running(app->karma_view_obj, true);
    }
}

bool wlan_app_scene_karma_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventKarmaToggle) {
            if(wlan_karma_is_running()) {
                wlan_karma_stop();
                wlan_karma_view_set_running(app->karma_view_obj, false);
            } else {
                uint8_t ch = app->channel_action_channel ? app->channel_action_channel : 6;
                wlan_karma_view_set_channel(app->karma_view_obj, ch);
                if(wlan_karma_start(ch, NULL, NULL)) {
                    wlan_karma_view_set_running(app->karma_view_obj, true);
                }
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(wlan_karma_is_running()) {
            wlan_karma_view_set_probe_reqs(app->karma_view_obj, wlan_karma_get_probe_reqs());
            wlan_karma_view_set_probe_resps(app->karma_view_obj, wlan_karma_get_probe_resps());
            wlan_karma_view_set_ssid_count(app->karma_view_obj, wlan_karma_get_ssid_count());
            wlan_karma_view_set_clients(app->karma_view_obj, wlan_karma_get_clients());
            char ssid_buf[33];
            if(wlan_karma_get_latest_probe_ssid(ssid_buf, sizeof(ssid_buf))) {
                wlan_karma_view_set_current_ssid(app->karma_view_obj, ssid_buf);
            }
        }
    }

    return consumed;
}

void wlan_app_scene_karma_on_exit(void* context) {
    WlanApp* app = context;
    wlan_karma_view_set_action_callback(app->karma_view_obj, NULL, NULL);
    wlan_karma_stop();
}
