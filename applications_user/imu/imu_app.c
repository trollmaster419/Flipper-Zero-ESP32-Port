#include "imu_app.h"
#include "imu_hal.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>

#define REFRESH_MS 100

static void render_cb(Canvas* const canvas, void* ctx) {
    ImuAppState* st = ctx;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(furi_mutex_acquire(st->mutex, 100) != FuriStatusOk) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 30, "Loading...");
        return;
    }

    ImuData d = {0};
    bool ok = (imu_get_type() != ImuTypeNone) && imu_read(&d);

    /* Header */
    canvas_set_font(canvas, FontPrimary);
    if(imu_get_type() != ImuTypeNone) {
        char hdr[48];
        snprintf(hdr, sizeof(hdr), "IMU: %s", imu_get_type_name());
        canvas_draw_str(canvas, 4, 10, hdr);
        char tstr[16];
        snprintf(tstr, sizeof(tstr), "T: %.1fC", ok ? (double)d.temperature : -1.0);
        canvas_draw_str(canvas, 170, 10, ok ? tstr : "T: --.-C");
    } else {
        canvas_draw_str(canvas, 4, 10, "IMU: Not detected");
    }

    /* Separator */
    canvas_draw_line(canvas, 0, 16, 239, 16);
    canvas_draw_line(canvas, 0, 17, 239, 17);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 10, 30, "Accel (G)");
    canvas_draw_str(canvas, 130, 30, "Gyro (deg/s)");

    if(ok) {
        char buf[24];
        snprintf(buf, sizeof(buf), "X: %+6.2f", (double)d.accel_x);
        canvas_draw_str(canvas, 10, 46, buf);
        snprintf(buf, sizeof(buf), "X: %+6.1f", (double)d.gyro_x);
        canvas_draw_str(canvas, 130, 46, buf);
        snprintf(buf, sizeof(buf), "Y: %+6.2f", (double)d.accel_y);
        canvas_draw_str(canvas, 10, 62, buf);
        snprintf(buf, sizeof(buf), "Y: %+6.1f", (double)d.gyro_y);
        canvas_draw_str(canvas, 130, 62, buf);
        snprintf(buf, sizeof(buf), "Z: %+6.2f", (double)d.accel_z);
        canvas_draw_str(canvas, 10, 78, buf);
        snprintf(buf, sizeof(buf), "Z: %+6.1f", (double)d.gyro_z);
        canvas_draw_str(canvas, 130, 78, buf);
        /* Tilt dot */
        int dx = (int)(d.accel_x * -10.0f);
        int dy = (int)(d.accel_y * 10.0f);
        canvas_draw_disc(canvas, 230 + dx, 126 + dy, 4);
        canvas_draw_circle(canvas, 230, 126, 6);
    } else {
        canvas_draw_str(canvas, 10, 46, "X:  --.--");
        canvas_draw_str(canvas, 130, 46, "X:  ---.-");
        canvas_draw_str(canvas, 10, 62, "Y:  --.--");
        canvas_draw_str(canvas, 130, 62, "Y:  ---.-");
        canvas_draw_str(canvas, 10, 78, "Z:  --.--");
        canvas_draw_str(canvas, 130, 78, "Z:  ---.-");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 130, 126, "Back = quit");
    furi_mutex_release(st->mutex);
}

static void input_cb(InputEvent* input_event, void* ctx) {
    PluginEvent ev = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(ctx, &ev, FuriWaitForever);
}

static void tick_cb(void* ctx) {
    PluginEvent ev = {.type = EventTypeTick};
    furi_message_queue_put(ctx, &ev, 0);
}

int32_t imu_app(void* p) {
    UNUSED(p);

    ImuAppState* st = malloc(sizeof(ImuAppState));
    st->event_queue = furi_message_queue_alloc(8, sizeof(PluginEvent));
    st->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!st->event_queue || !st->mutex) {
        if(st->event_queue) furi_message_queue_free(st->event_queue);
        if(st->mutex) furi_mutex_free(st->mutex);
        free(st);
        return 255;
    }

    imu_init();

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render_cb, st);
    view_port_input_callback_set(vp, input_cb, st->event_queue);

    FuriTimer* timer = furi_timer_alloc(tick_cb, FuriTimerTypePeriodic, st->event_queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);
    furi_timer_start(timer, REFRESH_MS);

    PluginEvent ev;
    for(bool run = true; run;) {
        if(furi_message_queue_get(st->event_queue, &ev, 100) != FuriStatusOk) continue;
        furi_mutex_acquire(st->mutex, FuriWaitForever);
        if(ev.type == EventTypeKey && ev.input.type == InputTypeShort &&
           ev.input.key == InputKeyBack)
            run = false;
        view_port_update(vp);
        furi_mutex_release(st->mutex);
    }

    furi_timer_free(timer);
    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(st->event_queue);
    furi_mutex_free(st->mutex);
    free(st);
    return 0;
}
