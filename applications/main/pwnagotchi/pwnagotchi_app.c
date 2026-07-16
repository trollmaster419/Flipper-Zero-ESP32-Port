#include "pwnagotchi_app.h"
#include "config.h"
#include "channel.h"
#include "pwnagotchi_detect.h"
#include "handshake.h"
#include "frame.h"
#include "deauth.h"
#include <gui/elements.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "../wlan_app/wlan_hal.h"
#define WORKER_STACK_SIZE 8192
#define EPOCH_INTERVAL_MS 100
static void draw_face_big(Canvas* canvas, int x, int y, PwnagotchiMood m) {
    int cx, cy;
    switch(m) {
    case MoodSleeping:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_dot(canvas, x + 6, y + 6);
        canvas_draw_dot(canvas, x + 8, y + 8);
        canvas_draw_line(canvas, x + 10, y + 8, x + 16, y + 8);
        canvas_draw_dot(canvas, x + 18, y + 8);
        canvas_draw_dot(canvas, x + 19, y + 7);
        canvas_draw_dot(canvas, x + 20, y + 6);
        canvas_draw_dot(canvas, x + 21, y + 5);
        break;
    case MoodAwakening:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_line(canvas, x + 4, y + 6, x + 7, y + 6);
        canvas_draw_line(canvas, x + 4, y + 7, x + 7, y + 7);
        canvas_draw_line(canvas, x + 19, y + 6, x + 22, y + 6);
        canvas_draw_line(canvas, x + 19, y + 7, x + 22, y + 7);
        canvas_draw_line(canvas, x + 10, y + 9, x + 16, y + 9);
        break;
    case MoodAwake:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        for (cy = y + 4; cy <= y + 7; cy++)
            for (cx = x + 4; cx <= x + 7; cx++)
                canvas_draw_dot(canvas, cx, cy);
        for (cy = y + 4; cy <= y + 7; cy++)
            for (cx = x + 19; cx <= x + 22; cx++)
                canvas_draw_dot(canvas, cx, cy);
        for (cx = x + 10; cx <= x + 16; cx++)
            canvas_draw_dot(canvas, cx, y + 8);
        canvas_draw_dot(canvas, x + 10, y + 9);
        canvas_draw_dot(canvas, x + 16, y + 9);
        break;
    case MoodObserving:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 4);
        canvas_draw_dot(canvas, x + 6, y + 5);
        canvas_draw_dot(canvas, x + 5, y + 6);
        canvas_draw_dot(canvas, x + 6, y + 7);
        canvas_draw_dot(canvas, x + 20, y + 4);
        canvas_draw_dot(canvas, x + 21, y + 5);
        canvas_draw_dot(canvas, x + 20, y + 6);
        canvas_draw_dot(canvas, x + 21, y + 7);
        canvas_draw_line(canvas, x + 10, y + 10, x + 16, y + 10);
        break;
    case MoodObservingHappy:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_box(canvas, x + 5, y + 4, 2, 3);
        canvas_draw_box(canvas, x + 20, y + 4, 2, 3);
        canvas_draw_line(canvas, x + 10, y + 10, x + 16, y + 10);
        canvas_draw_dot(canvas, x + 9, y + 10);
        canvas_draw_dot(canvas, x + 17, y + 10);
        break;
    case MoodIntense:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_dot(canvas, x + 6, y + 5);
        canvas_draw_dot(canvas, x + 20, y + 5);
        canvas_draw_dot(canvas, x + 21, y + 5);
        canvas_draw_box(canvas, x + 10, y + 7, 6, 4);
        break;
    case MoodCool:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_line(canvas, x + 4, y + 6, x + 7, y + 6);
        for (cy = y + 4; cy <= y + 7; cy++)
            canvas_draw_dot(canvas, x + 19, cy);
        canvas_draw_line(canvas, x + 11, y + 9, x + 15, y + 9);
        break;
    case MoodHappy:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 4);
        canvas_draw_dot(canvas, x + 6, y + 5);
        canvas_draw_dot(canvas, x + 20, y + 4);
        canvas_draw_dot(canvas, x + 21, y + 5);
        canvas_draw_dot(canvas, x + 11, y + 8);
        canvas_draw_dot(canvas, x + 12, y + 9);
        canvas_draw_dot(canvas, x + 13, y + 9);
        canvas_draw_dot(canvas, x + 14, y + 9);
        canvas_draw_dot(canvas, x + 15, y + 8);
        break;
    case MoodGrateful:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_dot(canvas, x + 6, y + 5);
        canvas_draw_dot(canvas, x + 20, y + 5);
        canvas_draw_dot(canvas, x + 21, y + 5);
        canvas_draw_line(canvas, x + 10, y + 9, x + 16, y + 9);
        canvas_draw_dot(canvas, x + 9, y + 9);
        canvas_draw_dot(canvas, x + 17, y + 9);
        break;
    case MoodExcited:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_dot(canvas, x + 6, y + 4);
        canvas_draw_dot(canvas, x + 20, y + 5);
        canvas_draw_dot(canvas, x + 21, y + 4);
        canvas_draw_box(canvas, x + 11, y + 7, 4, 2);
        canvas_draw_dot(canvas, x + 12, y + 9);
        canvas_draw_dot(canvas, x + 13, y + 10);
        canvas_draw_dot(canvas, x + 14, y + 9);
        break;
    case MoodSmart:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_box(canvas, x + 4, y + 4, 4, 4);
        canvas_draw_box(canvas, x + 19, y + 4, 4, 4);
        canvas_draw_line(canvas, x + 10, y + 9, x + 16, y + 9);
        break;
    case MoodFriendly:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_dot(canvas, x + 6, y + 5);
        canvas_draw_dot(canvas, x + 20, y + 5);
        canvas_draw_dot(canvas, x + 21, y + 5);
        canvas_draw_dot(canvas, x + 12, y + 7);
        canvas_draw_dot(canvas, x + 13, y + 7);
        canvas_draw_dot(canvas, x + 11, y + 8);
        canvas_draw_dot(canvas, x + 14, y + 8);
        canvas_draw_dot(canvas, x + 12, y + 9);
        canvas_draw_dot(canvas, x + 13, y + 9);
        break;
    case MoodMotivated:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        for (cy = y + 4; cy <= y + 7; cy++)
            for (cx = x + 4; cx <= x + 7; cx++)
                canvas_draw_dot(canvas, cx, cy);
        for (cy = y + 4; cy <= y + 7; cy++)
            for (cx = x + 19; cx <= x + 22; cx++)
                canvas_draw_dot(canvas, cx, cy);
        canvas_draw_dot(canvas, x + 11, y + 7);
        canvas_draw_dot(canvas, x + 12, y + 8);
        canvas_draw_dot(canvas, x + 13, y + 8);
        canvas_draw_dot(canvas, x + 14, y + 8);
        canvas_draw_dot(canvas, x + 15, y + 7);
        break;
    case MoodDemotivated:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_line(canvas, x + 4, y + 6, x + 7, y + 6);
        canvas_draw_line(canvas, x + 4, y + 7, x + 7, y + 7);
        canvas_draw_line(canvas, x + 19, y + 6, x + 22, y + 6);
        canvas_draw_line(canvas, x + 19, y + 7, x + 22, y + 7);
        canvas_draw_line(canvas, x + 10, y + 10, x + 16, y + 10);
        break;
    case MoodBored:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_line(canvas, x + 4, y + 6, x + 7, y + 6);
        canvas_draw_line(canvas, x + 19, y + 6, x + 22, y + 6);
        canvas_draw_line(canvas, x + 10, y + 10, x + 16, y + 10);
        break;
    case MoodSad:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_line(canvas, x + 4, y + 5, x + 7, y + 5);
        canvas_draw_line(canvas, x + 19, y + 5, x + 22, y + 5);
        canvas_draw_dot(canvas, x + 5, y + 6);
        canvas_draw_dot(canvas, x + 6, y + 6);
        canvas_draw_dot(canvas, x + 20, y + 6);
        canvas_draw_dot(canvas, x + 21, y + 6);
        canvas_draw_line(canvas, x + 11, y + 10, x + 15, y + 10);
        canvas_draw_dot(canvas, x + 10, y + 9);
        canvas_draw_dot(canvas, x + 16, y + 9);
        break;
    case MoodLonely:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_dot(canvas, x + 5, y + 5);
        canvas_draw_line(canvas, x + 4, y + 6, x + 7, y + 6);
        canvas_draw_dot(canvas, x + 20, y + 5);
        canvas_draw_line(canvas, x + 19, y + 6, x + 22, y + 6);
        canvas_draw_dot(canvas, x + 12, y + 8);
        canvas_draw_dot(canvas, x + 13, y + 9);
        canvas_draw_dot(canvas, x + 14, y + 10);
        canvas_draw_dot(canvas, x + 12, y + 10);
        break;
    case MoodBroken:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_line(canvas, x + 4, y + 4, x + 7, y + 7);
        canvas_draw_line(canvas, x + 4, y + 7, x + 7, y + 4);
        canvas_draw_line(canvas, x + 19, y + 4, x + 22, y + 7);
        canvas_draw_line(canvas, x + 19, y + 7, x + 22, y + 4);
        canvas_draw_dot(canvas, x + 11, y + 8);
        canvas_draw_line(canvas, x + 12, y + 9, x + 14, y + 9);
        break;
    case MoodDebug:
        canvas_draw_line(canvas, x, y + 4, x, y + 12);
        canvas_draw_line(canvas, x + 26, y + 4, x + 26, y + 12);
        canvas_draw_box(canvas, x + 4, y + 4, 4, 4);
        canvas_draw_box(canvas, x + 19, y + 4, 4, 4);
        canvas_draw_line(canvas, x + 4, y + 4, x + 8, y + 8);
        canvas_draw_line(canvas, x + 4, y + 7, x + 8, y + 4);
        canvas_draw_line(canvas, x + 19, y + 4, x + 23, y + 8);
        canvas_draw_line(canvas, x + 19, y + 7, x + 23, y + 4);
        canvas_draw_line(canvas, x + 10, y + 10, x + 16, y + 10);
        break;
    }
}
// Word-wrap `text` into a column [x, x+max_w) starting at baseline y,
// drawing at most `max_lines` lines spaced `line_h` apart.
static void draw_wrapped(
    Canvas* canvas, int x, int y, int max_w, int line_h, int max_lines, const char* text) {
    char line[40];
    int line_len = 0;
    int lines_drawn = 0;
    const char* p = text;
    while(*p && lines_drawn < max_lines) {
        // grab next word (including a leading space)
        const char* word = p;
        while(*p == ' ') p++;
        const char* wstart = p;
        while(*p && *p != ' ') p++;
        int wlen = p - word; // includes leading spaces
        char candidate[40];
        int clen = line_len + wlen;
        if(clen >= (int)sizeof(candidate)) clen = sizeof(candidate) - 1;
        memcpy(candidate, line, line_len);
        memcpy(candidate + line_len, word, clen - line_len);
        candidate[clen] = '\0';
        if(line_len > 0 && canvas_string_width(canvas, candidate) > max_w) {
            // flush current line, start new one with this word (no leading space)
            line[line_len] = '\0';
            canvas_draw_str(canvas, x, y + lines_drawn * line_h, line);
            lines_drawn++;
            line_len = 0;
            int copy = wstart == word ? wlen : (int)(p - wstart);
            if(copy >= (int)sizeof(line)) copy = sizeof(line) - 1;
            memcpy(line, wstart, copy);
            line_len = copy;
        } else {
            memcpy(line, candidate, clen);
            line_len = clen;
        }
    }
    if(line_len > 0 && lines_drawn < max_lines) {
        line[line_len] = '\0';
        canvas_draw_str(canvas, x, y + lines_drawn * line_h, line);
    }
}

static void render_cb(Canvas* const canvas, void* ctx) {
    PwnagotchiState* s = ctx;
    if(furi_mutex_acquire(s->mutex, 200) != FuriStatusOk) return;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    int w = canvas_width(canvas);
    char buf[128];

    canvas_set_font(canvas, FontSecondary);

    // ---- Top status bar: CH nn  APS n (nn)   BT -   UP hh:mm:ss ----
    snprintf(buf, sizeof(buf), "CH %02d", s->current_channel);
    canvas_draw_str(canvas, 2, 7, buf);
    int x = 2 + canvas_string_width(canvas, buf) + 6;

    snprintf(buf, sizeof(buf), "APS %d (%02d)", s->aps_now, s->aps_total);
    canvas_draw_str(canvas, x, 7, buf);
    int left_end = x + canvas_string_width(canvas, buf);

    unsigned long up = s->uptime_secs;
    snprintf(buf, sizeof(buf), "UP %02lu:%02lu:%02lu",
             up / 3600UL, (up / 60UL) % 60UL, up % 60UL);
    int up_w = canvas_string_width(canvas, buf);
    canvas_draw_str(canvas, w - up_w - 2, 7, buf);
    int right_start = w - up_w - 2;

    // BT indicator only if it fits between the two groups
    const char* bt = "BT -";
    int bt_w = canvas_string_width(canvas, bt);
    if(right_start - left_end > bt_w + 8) {
        canvas_draw_str(canvas, (left_end + right_start - bt_w) / 2, 7, bt);
    }
    canvas_draw_line(canvas, 0, 10, w, 10);

    // ---- Name (top-left) ----
    snprintf(buf, sizeof(buf), "%s>", g_config.name);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 22, buf);
    canvas_set_font(canvas, FontSecondary);

    // ---- Face (lower-left) ----
    draw_face_big(canvas, 2, 30, s->mood);

    // ---- Speech text (right of the face) ----
    if(s->pwnagotchi_detected) {
        snprintf(buf, sizeof(buf), "%s: %s", s->epoch_status, s->peer_name);
    } else if(s->ap_name[0]) {
        snprintf(buf, sizeof(buf), "%s (%.16s)", s->epoch_status, s->ap_name);
    } else {
        snprintf(buf, sizeof(buf), "%s", s->epoch_status);
    }
    draw_wrapped(canvas, 34, 30, w - 34 - 2, 10, 3, buf);

    canvas_draw_line(canvas, 0, 53, w, 53);

    // ---- Bottom bar: PWND n (nn)   ...   AUTO ----
    snprintf(buf, sizeof(buf), "PWND %lu (%02lu)",
             (unsigned long)s->pwnd_run, (unsigned long)s->pwnd_tot);
    canvas_draw_str(canvas, 2, 63, buf);
    const char* mode = "AUTO";
    canvas_draw_str(canvas, w - canvas_string_width(canvas, mode) - 2, 63, mode);
    furi_mutex_release(s->mutex);
}
static void input_cb(InputEvent* ev, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* q = ctx;
    PluginEvent e = {.type = EventTypeKey, .input = *ev};
    furi_message_queue_put(q, &e, FuriWaitForever);
}
static void tick_cb(void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* q = ctx;
    PluginEvent e = {.type = EventTypeTick};
    furi_message_queue_put(q, &e, 0);
}
static void post_event(FuriMessageQueue* q, EventType t) {
    PluginEvent e = {.type = t};
    furi_message_queue_put(q, &e, 0);
}
static void worker_task(void* param) {
    PwnagotchiState* state = (PwnagotchiState*)param;
    FuriMessageQueue* q = state->event_queue;
    if(!wlan_hal_start()) {
        ESP_LOGE(TAG, "WiFi init failed");
        post_event(q, EventTypeWifiError);
        state->worker_running = false;
        vTaskDelete(NULL);
        return;
    }
    wlan_hal_set_promiscuous(true, pwnagotchi_promisc_cb);
    wlan_hal_set_channel((uint8_t)g_config.init_channel);
    state->wifi_started = true;
    state->mood = MoodAwake;
    state->epochs_since_peer = 0;
    state->epochs_total = 0;
    state->handshakes_this_session = 0;
    state->missed_deauths = 0;
    post_event(q, EventTypeEpochDone);
    int epoch_step = 0;
    while(state->worker_running) {
        g_config.uptime = config_uptime_secs();
        switch(epoch_step) {
        case 0:
            channel_cycle();
            {
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                state->current_channel = channel_get();
                strncpy(state->epoch_status, "zzZ *snrt*", sizeof(state->epoch_status) - 1);
                state->mood = (state->epochs_total < 2) ? MoodAwakening : MoodSleeping;
                furi_mutex_release(state->mutex);
            }
            post_event(q, EventTypeEpochDone);
            furi_delay_ms(g_config.short_delay_ms);
            break;
        case 1:
            {
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                strncpy(state->epoch_status, "who dat?", sizeof(state->epoch_status) - 1);
                state->mood = MoodObserving;
                furi_mutex_release(state->mutex);
            }
            post_event(q, EventTypeEpochDone);
            pwnagotchi_detect();
            if(g_pwnagotchi_detected) {
                state->epochs_since_peer = 0;
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                state->pwnagotchi_detected = true;
                int r = config_random(0, 100);
                if (r < 15) {
                    state->mood = MoodExcited;
                    strncpy(state->epoch_status, "omg a fren!", sizeof(state->epoch_status) - 1);
                } else if (r < 30) {
                    state->mood = MoodGrateful;
                    strncpy(state->epoch_status, "ty for visit!", sizeof(state->epoch_status) - 1);
                } else if (r < 45 && state->pwnd_tot > 0) {
                    state->mood = MoodSmart;
                    strncpy(state->epoch_status, "i know things", sizeof(state->epoch_status) - 1);
                } else if (r < 60) {
                    state->mood = MoodFriendly;
                    strncpy(state->epoch_status, "hai there!", sizeof(state->epoch_status) - 1);
                } else {
                    state->mood = MoodObservingHappy;
                    strncpy(state->epoch_status, "yay a fren!", sizeof(state->epoch_status) - 1);
                }
                strncpy(state->peer_name, g_last_peer.name, sizeof(state->peer_name) - 1);
                strncpy(state->peer_type, g_last_peer.device_type, sizeof(state->peer_type) - 1);
                strncpy(state->peer_pwnd, g_last_peer.pwnd_tot, sizeof(state->peer_pwnd) - 1);
                furi_mutex_release(state->mutex);
                post_event(q, EventTypePwnDetected);
            } else {
                state->epochs_since_peer++;
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                state->pwnagotchi_detected = false;
                if(state->epochs_since_peer > g_config.sad_num_epochs) {
                    if (state->missed_deauths >= g_config.max_misses_for_recon) {
                        state->mood = MoodLonely;
                        strncpy(state->epoch_status, "all alone...", sizeof(state->epoch_status) - 1);
                    } else {
                        state->mood = MoodSad;
                        strncpy(state->epoch_status, "miss frens...", sizeof(state->epoch_status) - 1);
                    }
                } else if(state->epochs_since_peer > g_config.bored_num_epochs) {
                    state->mood = MoodBored;
                    strncpy(state->epoch_status, "so boring", sizeof(state->epoch_status) - 1);
                } else {
                    state->mood = MoodObserving;
                    strncpy(state->epoch_status, "no frens yet", sizeof(state->epoch_status) - 1);
                }
                memset(state->peer_name, 0, sizeof(state->peer_name));
                furi_mutex_release(state->mutex);
                post_event(q, EventTypePwnLost);
            }
            break;
        case 2:
            if(!g_config.deauth) {
                state->mood = MoodAwake;
                break;
            }
            {
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                strncpy(state->epoch_status, "KILL AP KILL", sizeof(state->epoch_status) - 1);
                state->mood = MoodIntense;
                furi_mutex_release(state->mutex);
            }
            post_event(q, EventTypeEpochDone);
            handshake_start_capture();
            deauth_run();
            {
                char target_essid[33];
                uint8_t target_bssid[6];
                deauth_get_target(target_essid, sizeof(target_essid), target_bssid);
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                deauth_get_ap_counts(&state->aps_now, &state->aps_total);
                state->mood = MoodCool;
                strncpy(state->epoch_status, "pwnd!", sizeof(state->epoch_status) - 1);
                strncpy(state->ap_name, target_essid, sizeof(state->ap_name) - 1);
                memcpy(state->ap_bssid, target_bssid, 6);
                furi_mutex_release(state->mutex);
            }
            furi_delay_ms(2000);
            post_event(q, EventTypeDeauthDone);
            break;
        case 3:
            if(!g_config.advertise) {
                state->mood = MoodAwake;
                break;
            }
            {
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                strncpy(state->epoch_status, "heya new frens", sizeof(state->epoch_status) - 1);
                state->mood = MoodCool;
                furi_mutex_release(state->mutex);
            }
            post_event(q, EventTypeEpochDone);
            frame_advertise();
            {
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                state->pwnd_run = (uint32_t)g_config.pwnd_run;
                state->pwnd_tot = (uint32_t)g_config.pwnd_tot;
                if(state->epochs_since_peer < 3) {
                    state->mood = MoodHappy;
                    strncpy(state->epoch_status, "yay a fren!", sizeof(state->epoch_status) - 1);
                } else if(state->handshakes_this_session > 0) {
                    state->mood = MoodMotivated;
                    strncpy(state->epoch_status, "on fire!", sizeof(state->epoch_status) - 1);
                } else if(config_random(0, 100) < 15) {
                    state->mood = MoodExcited;
                    strncpy(state->epoch_status, "ooh what's that?", sizeof(state->epoch_status) - 1);
                } else if(config_random(0, 100) < 10) {
                    state->mood = MoodSmart;
                    strncpy(state->epoch_status, "i am smart", sizeof(state->epoch_status) - 1);
                } else if(state->epochs_since_peer > g_config.sad_num_epochs) {
                } else if(state->epochs_since_peer > g_config.bored_num_epochs) {
                } else {
                    state->mood = MoodAwake;
                    strncpy(state->epoch_status, "just chillin", sizeof(state->epoch_status) - 1);
                }
                furi_mutex_release(state->mutex);
            }
            post_event(q, EventTypeAdvertDone);
            break;
        default:
            break;
        }
        epoch_step = (epoch_step + 1) % 4;
        state->epochs_total++;
        furi_delay_ms(EPOCH_INTERVAL_MS);
    }
    wlan_hal_set_promiscuous(false, NULL);
    wlan_hal_stop();
    vTaskDelete(NULL);
}
int32_t pwnagotchi_app(void* p) {
    UNUSED(p);
    PwnagotchiState* state = malloc(sizeof(PwnagotchiState));
    memset(state, 0, sizeof(*state));
    state->mood = MoodAwake;
    state->event_queue = furi_message_queue_alloc(16, sizeof(PluginEvent));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    config_init();
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render_cb, state);
    view_port_input_callback_set(vp, input_cb, state->event_queue);
    FuriTimer* timer = furi_timer_alloc(tick_cb, FuriTimerTypePeriodic, state->event_queue);
    furi_timer_start(timer, furi_kernel_get_tick_frequency());
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);
    wlan_hal_bt_stop();
    state->worker_running = true;
    // Try internal RAM first (for flash safety), fall back to PSRAM
    size_t stack_size = WORKER_STACK_SIZE;
    StackType_t* stack = heap_caps_malloc(stack_size * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!stack) {
        stack = heap_caps_malloc(stack_size * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    TaskHandle_t worker_handle;
    if(stack) {
        static StaticTask_t worker_tcb;
        worker_handle = xTaskCreateStatic(worker_task, "PwnWorker", stack_size, state, 5, stack, &worker_tcb);
    } else {
        ESP_LOGE("PwnApp", "Cannot alloc worker stack");
        worker_handle = NULL;
    }
    PluginEvent event;
    bool running = true;
    while(running) {
        FuriStatus s = furi_message_queue_get(state->event_queue, &event, 100);
        if(s != FuriStatusOk) continue;
        if(furi_mutex_acquire(state->mutex, FuriWaitForever) != FuriStatusOk) continue;
        switch(event.type) {
        case EventTypeKey:
            if(event.input.type == InputTypeShort && event.input.key == InputKeyBack) {
                running = false;
            }
            break;
        case EventTypeTick:
            state->uptime_secs++;
            break;
        case EventTypePwnDetected:
        case EventTypePwnLost:
        case EventTypeAdvertDone:
        case EventTypeEpochDone:
            state->current_channel = channel_get();
            break;
        case EventTypeDeauthDone: {
            state->current_channel = channel_get();
            uint32_t new_hs = handshake_process(
                state->ap_name, state->ap_bssid);
            if (new_hs == 0xFFFFFFFF) {
                strncpy(state->epoch_status, "Missed 4way", sizeof(state->epoch_status) - 1);
                state->mood = MoodDemotivated;
                state->missed_deauths++;
                memset(state->ap_name, 0, sizeof(state->ap_name));
                memset(state->ap_bssid, 0, sizeof(state->ap_bssid));
                view_port_update(vp);
                furi_mutex_release(state->mutex);
                furi_delay_ms(1000);
                if (furi_mutex_acquire(state->mutex, FuriWaitForever) != FuriStatusOk) continue;
                break;
            } else if(new_hs > 0) {
                state->pwnd_run += new_hs;
                state->pwnd_tot += new_hs;
                g_config.pwnd_run += new_hs;
                g_config.pwnd_tot += new_hs;
                state->handshakes_this_session += (int)new_hs;
                state->mood = MoodHappy;
                snprintf(state->epoch_status, sizeof(state->epoch_status),
                         "got %lu!", (unsigned long)new_hs);
                ESP_LOGI(TAG, "Captured %lu handshake(s)!", (unsigned long)new_hs);
            }
            memset(state->ap_name, 0, sizeof(state->ap_name));
            memset(state->ap_bssid, 0, sizeof(state->ap_bssid));
            break;
        }
        case EventTypeWifiError:
            ESP_LOGE(TAG, "WiFi error, shutting down");
            running = false;
            break;
        }
        view_port_update(vp);
        furi_mutex_release(state->mutex);
    }
    state->worker_running = false;
    furi_delay_ms(200);
    furi_timer_free(timer);
    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    wlan_hal_bt_restore();
    view_port_free(vp);
    furi_message_queue_free(state->event_queue);
    furi_mutex_free(state->mutex);
    free(state);
    handshake_deinit();
    return 0;
}