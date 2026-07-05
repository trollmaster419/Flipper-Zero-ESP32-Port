#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <math.h>
#include "minecraft_app.h"

#define TAG "Minecraft"

typedef struct {
    float x, y, z;
} Vector3;

typedef struct {
    Vector3 pos;
    float yaw, pitch;
} Player;

typedef struct {
    Player player;
    FuriMutex* mutex;
    bool running;
} MinecraftApp;

static void minecraft_draw_callback(Canvas* canvas, void* ctx) {
    MinecraftApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    
    // Render a few blocks relative to the player
    render_cube(canvas, 
        2.0f - app->player.pos.x, 
        0.0f - app->player.pos.y, 
        5.0f - app->player.pos.z, 
        app->player.yaw, app->player.pitch, 1.0f);

    render_cube(canvas, 
        -2.0f - app->player.pos.x, 
        0.0f - app->player.pos.y, 
        8.0f - app->player.pos.z, 
        app->player.yaw, app->player.pitch, 1.0f);
    
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 10, 20, "Minecraft ESP32");
    
    char buf[32];
    snprintf(buf, sizeof(buf), "X:%.1f Y:%.1f Z:%.1f", 
             (double)app->player.pos.x, (double)app->player.pos.y, (double)app->player.pos.z);
    canvas_draw_str(canvas, 10, 40, buf);

    furi_mutex_release(app->mutex);
}

static void minecraft_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

int32_t minecraft_app(void* p) {
    UNUSED(p);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    MinecraftApp* app = malloc(sizeof(MinecraftApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->running = true;
    
    app->player.pos = (Vector3){0, 0, 0};
    app->player.yaw = 0;
    app->player.pitch = 0;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, minecraft_draw_callback, app);
    view_port_input_callback_set(view_port, minecraft_input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool ok_pressed = false;
    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) {
                app->running = false;
            }

            if(event.key == InputKeyOk) {
                if(event.type == InputTypePress) ok_pressed = true;
                if(event.type == InputTypeRelease) ok_pressed = false;
            }
            
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(ok_pressed) {
                // Rotate
                if(event.key == InputKeyUp) app->player.pitch += 0.1f;
                if(event.key == InputKeyDown) app->player.pitch -= 0.1f;
                if(event.key == InputKeyLeft) app->player.yaw -= 0.1f;
                if(event.key == InputKeyRight) app->player.yaw += 0.1f;
            } else {
                // Move
                if(event.key == InputKeyUp) app->player.pos.z += 0.5f;
                if(event.key == InputKeyDown) app->player.pos.z -= 0.5f;
                if(event.key == InputKeyLeft) app->player.pos.x -= 0.5f;
                if(event.key == InputKeyRight) app->player.pos.x += 0.5f;
            }
            furi_mutex_release(app->mutex);
        }
        view_port_update(view_port);
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(app->mutex);
    free(app);
    furi_record_close(RECORD_GUI);

    return 0;
}
