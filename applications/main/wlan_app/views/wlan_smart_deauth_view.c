#include "wlan_smart_deauth_view.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <furi.h>
#include <stdio.h>

static void wlan_smart_deauth_view_draw(Canvas* canvas, void* model) {
    WlanSmartDeauthModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Smart Deauth");

    canvas_set_font(canvas, FontSecondary);
    char buf[32];
    snprintf(buf, sizeof(buf), "Frames: %lu", (unsigned long)m->frames_sent);
    canvas_draw_str(canvas, 2, 24, buf);

    if (m->running) {
        canvas_draw_str(canvas, 72, 12, "ATTACKING");
    } else {
        canvas_draw_str(canvas, 85, 12, "READY");
    }

    const int bar_y = 40;
    const int ch_width = 9;
    const int box_size = 6;
    const int box_y = 51;

    for (int ch = 1; ch <= 13; ch++) {
        int x = 3 + (ch - 1) * ch_width;

        if (m->ap_counts[ch] > 0) {
            bool is_target = false;
            for (uint8_t i = 0; i < m->active_count; i++) {
                if (m->active_channels[i] == ch) {
                    is_target = true;
                    break;
                }
            }

            if (is_target) {
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_box(canvas, x, box_y - box_size, box_size, box_size);
                canvas_set_color(canvas, ColorBlack);
                for (int dy = 0; dy < box_size; dy++) {
                    for (int dx = 0; dx < box_size; dx++) {
                        if ((dx + dy) % 2 == 0) {
                            canvas_draw_dot(canvas, x + dx, box_y - box_size + dy);
                        }
                    }
                }
                canvas_draw_frame(canvas, x-1, box_y - box_size -1, box_size+2, box_size+2);
            } else {
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_box(canvas, x, box_y - box_size, box_size, box_size);
            }
        }
    }

    canvas_draw_line(canvas, 0, bar_y + 14, 128, bar_y + 14);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, "1 2 3 4 5 6 7 8 9 0 1 2 3");
}

static bool wlan_smart_deauth_view_input(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if (!vd) return false;

    if (event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(vd, WlanAppCustomEventDeautherStart);
        return true;
    }
    return false;
}

View* wlan_smart_deauth_view_alloc(void) {
    View* view = view_alloc();
    if (!view) return NULL;

    view_allocate_model(view, ViewModelTypeLockFree, sizeof(WlanSmartDeauthModel));

    WlanSmartDeauthModel* model = view_get_model(view);
    memset(model, 0, sizeof(WlanSmartDeauthModel));

    view_set_draw_callback(view, wlan_smart_deauth_view_draw);
    view_set_input_callback(view, wlan_smart_deauth_view_input);

    return view;
}

void wlan_smart_deauth_view_free(View* view) {
    if (view) {
        view_free(view);
    }
}