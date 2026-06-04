#include "wlan_karma_view.h"
#include "wlan_view_common.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint32_t probe_reqs;
    uint32_t probe_resps;
    uint16_t ssid_count;
    uint16_t clients;
    uint8_t channel;
    bool running;
    char current_ssid[33];
} WlanKarmaViewModel;

struct WlanKarmaView {
    View* view;
    WlanKarmaViewActionCb action_cb;
    void* action_ctx;
};

static void format_num(uint32_t n, char* buf, size_t sz) {
    if(n < 1000) {
        snprintf(buf, sz, "%lu", (unsigned long)n);
    } else if(n < 1000000) {
        snprintf(buf, sz, "%lu.%luk",
                 (unsigned long)(n / 1000),
                 (unsigned long)((n % 1000) / 100));
    } else {
        snprintf(buf, sz, "%lu.%luM",
                 (unsigned long)(n / 1000000),
                 (unsigned long)((n % 1000000) / 100000));
    }
}

static void wlan_karma_view_draw(Canvas* canvas, void* model) {
    WlanKarmaViewModel* m = model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, "Karma Attack");

    if(m->channel) {
        canvas_set_font(canvas, FontSecondary);
        char ch_buf[20];
        snprintf(ch_buf, sizeof(ch_buf), "Ch:%u", (unsigned)m->channel);
        uint16_t cw = canvas_string_width(canvas, ch_buf);
        canvas_draw_str(canvas, 128 - 3 - cw, WLAN_VIEW_HEADER_BASELINE_Y, ch_buf);
    }

    canvas_draw_icon(canvas, 0, 14, &I_WarningDolphin_45x42);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 86, 18, AlignCenter, AlignBottom, "Karma");

    canvas_set_font(canvas, FontSecondary);
    char buf[48];

    if(m->current_ssid[0]) {
        snprintf(buf, sizeof(buf), "SSID: %.26s", m->current_ssid);
        canvas_draw_str_aligned(canvas, 86, 28, AlignCenter, AlignBottom, buf);
    }

    snprintf(buf, sizeof(buf), "Probe Reqs:  ");
    canvas_draw_str_aligned(canvas, 86, 36, AlignCenter, AlignBottom, buf);
    format_num(m->probe_reqs, buf, sizeof(buf));
    canvas_draw_str_aligned(canvas, 86, 44, AlignCenter, AlignBottom, buf);

    snprintf(buf, sizeof(buf), "Probe Resps: ");
    canvas_draw_str_aligned(canvas, 86, 52, AlignCenter, AlignBottom, buf);
    format_num(m->probe_resps, buf, sizeof(buf));
    canvas_draw_str_aligned(canvas, 86, 60, AlignCenter, AlignBottom, buf);

    snprintf(buf, sizeof(buf), "SSIDs: %u", (unsigned)m->ssid_count);
    canvas_draw_str_aligned(canvas, 0, 52, AlignLeft, AlignBottom, buf);

    snprintf(buf, sizeof(buf), "Clients: %u", (unsigned)m->clients);
    canvas_draw_str_aligned(canvas, 0, 60, AlignLeft, AlignBottom, buf);

    elements_button_right(canvas, m->running ? "Stop" : "Start");
}

static bool wlan_karma_view_input(InputEvent* event, void* context) {
    WlanKarmaView* v = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyDown) {
        if(v->action_cb) v->action_cb(v->action_ctx);
        return true;
    }
    return false;
}

WlanKarmaView* wlan_karma_view_alloc(void) {
    WlanKarmaView* v = malloc(sizeof(WlanKarmaView));
    v->view = view_alloc();
    v->action_cb = NULL;
    v->action_ctx = NULL;
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(WlanKarmaViewModel));
    view_set_draw_callback(v->view, wlan_karma_view_draw);
    view_set_input_callback(v->view, wlan_karma_view_input);

    WlanKarmaViewModel* m = view_get_model(v->view);
    memset(m, 0, sizeof(*m));
    view_commit_model(v->view, false);

    return v;
}

void wlan_karma_view_free(WlanKarmaView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* wlan_karma_view_get_view(WlanKarmaView* v) {
    return v->view;
}

void wlan_karma_view_set_channel(WlanKarmaView* v, uint8_t channel) {
    with_view_model(v->view, WlanKarmaViewModel * m, { m->channel = channel; }, true);
}

void wlan_karma_view_set_probe_reqs(WlanKarmaView* v, uint32_t count) {
    with_view_model(v->view, WlanKarmaViewModel * m, { m->probe_reqs = count; }, true);
}

void wlan_karma_view_set_probe_resps(WlanKarmaView* v, uint32_t count) {
    with_view_model(v->view, WlanKarmaViewModel * m, { m->probe_resps = count; }, true);
}

void wlan_karma_view_set_ssid_count(WlanKarmaView* v, uint16_t count) {
    with_view_model(v->view, WlanKarmaViewModel * m, { m->ssid_count = count; }, true);
}

void wlan_karma_view_set_clients(WlanKarmaView* v, uint16_t count) {
    with_view_model(v->view, WlanKarmaViewModel * m, { m->clients = count; }, true);
}

void wlan_karma_view_set_running(WlanKarmaView* v, bool running) {
    with_view_model(v->view, WlanKarmaViewModel * m, { m->running = running; }, true);
}

void wlan_karma_view_set_action_callback(WlanKarmaView* v, WlanKarmaViewActionCb cb, void* ctx) {
    v->action_cb = cb;
    v->action_ctx = ctx;
}

void wlan_karma_view_set_current_ssid(WlanKarmaView* v, const char* ssid) {
    with_view_model(v->view, WlanKarmaViewModel * m, {
        strncpy(m->current_ssid, ssid ? ssid : "", sizeof(m->current_ssid) - 1);
        m->current_ssid[sizeof(m->current_ssid) - 1] = 0;
    }, true);
}
