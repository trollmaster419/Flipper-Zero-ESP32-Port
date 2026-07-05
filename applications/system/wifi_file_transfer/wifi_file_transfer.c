#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#include "wifi_file_transfer_hal.h"

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriTimer* timer;
    volatile bool running;
} WftApp;

static void wft_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    WftStatus st;
    wft_get_status(&st);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "WiFi File Share");

    canvas_set_font(canvas, FontSecondary);
    if(!st.running) {
        canvas_draw_str(canvas, 2, 26, "Error starting AP");
        return;
    }

    canvas_draw_str(canvas, 2, 24, "SSID: Flipper-FileShare");
    canvas_draw_str(canvas, 2, 36, "Open (no password)");

    FuriString* buf = furi_string_alloc();
    furi_string_printf(buf, "IP:   %s", st.ip);
    canvas_draw_str(canvas, 2, 48, furi_string_get_cstr(buf));

    furi_string_printf(buf, "Clients: %d", st.client_count);
    canvas_draw_str(canvas, 2, 60, furi_string_get_cstr(buf));
    furi_string_free(buf);

    canvas_draw_str(canvas, 2, 80, "Back = Stop & Exit");
}

static void wft_input_callback(InputEvent* event, void* context) {
    WftApp* app = context;
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        app->running = false;
    }
}

static void wft_timer_callback(void* context) {
    WftApp* app = context;
    if(app->running) {
        view_port_update(app->view_port);
    }
}

int32_t wifi_file_transfer_app(void* p) {
    UNUSED(p);

    WftApp* app = malloc(sizeof(WftApp));
    app->running = true;

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, wft_draw_callback, app);
    view_port_input_callback_set(app->view_port, wft_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    wft_start();
    view_port_update(app->view_port);

    app->timer = furi_timer_alloc(wft_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_kernel_get_tick_frequency());

    while(app->running) {
        furi_delay_ms(100);
    }

    furi_timer_free(app->timer);
    wft_stop();
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}
