#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view_port.h>
#include <momentum_settings/momentum_settings.h>
extern void cli_uart_bridge_start();
extern void cli_uart_bridge_stop();
void furi_log_set_console_muted(bool muted);
/* This is the function that draws the screen */
static void qflipper_app_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "qFlipper Bridge");
    
    canvas_set_font(canvas, FontSecondary);
    if(momentum_settings.qflipper_enabled) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, 0, 18, 128, 24, 3);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str(canvas, 5, 30, "Status: ENABLED");
        canvas_draw_str(canvas, 5, 40, "UART0 is in RPC Mode");
    } else {
        canvas_draw_str(canvas, 2, 30, "Status: Disabled");
        canvas_draw_str(canvas, 2, 40, "UART0 is in Logging Mode");
    }

    canvas_set_color(canvas, ColorBlack);
    elements_button_center(canvas, momentum_settings.qflipper_enabled ? "Disable" : "Enable");
    elements_button_left(canvas, "Exit");
}

/* This handles button presses */
static void qflipper_app_input_callback(InputEvent* event, void* context) {
    FuriMessageQueue* event_queue = context;
    furi_message_queue_put(event_queue, event, FuriWaitForever);
}

int32_t qflipper_app(void* p) {
    UNUSED(p);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, qflipper_app_draw_callback, NULL);
    view_port_input_callback_set(view_port, qflipper_app_input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    while(1) {
        if(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            // Exit on Back button or Left button
            if(event.type == InputTypeShort && (event.key == InputKeyBack || event.key == InputKeyLeft)) {
                break;
            }
            // Toggle on OK button
            // Toggle on OK button
            if(event.type == InputTypeShort && event.key == InputKeyOk) {
                momentum_settings.qflipper_enabled = !momentum_settings.qflipper_enabled;
                momentum_settings_save();
                
                furi_log_set_console_muted(momentum_settings.qflipper_enabled);
                
                if(momentum_settings.qflipper_enabled) {
                    cli_uart_bridge_start();
                    FURI_LOG_E("qFlipper", "Bridge Started");
                } else {
                    cli_uart_bridge_stop();
                    FURI_LOG_E("qFlipper", "Bridge Stopped");
                }
                
                view_port_update(view_port);
            }
        }
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    return 0;
}
