#pragma once

#include <gui/gui.h>
#include <storage/storage.h>
#include <desktop/desktop.h>
#include <desktop/views/desktop_view_slideshow.h>
#include <dialogs/dialogs.h>
#include <expansion/expansion.h>
#include <notification/notification_app.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <power/power_service/power.h>

#include <gui/modules/variable_item_list.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/number_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/dialog_ex.h>

#include <momentum_settings/asset_packs.h>
#include <momentum_settings/namespoof.h>
#include <momentum_settings/momentum_settings.h>
#include <loader/loader_menu.h>
#include <dolphin/dolphin.h>
#include <dolphin/dolphin_i.h>
#include <dolphin/helpers/dolphin_state.h>

#include <applications.h>
#include <assets_icons.h>
#include <flipper_application/flipper_application.h>
#include <furi.h>
#include <furi_hal.h>
#include <notification/notification_messages.h>
#include <gui/view.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/value_index.h>
#include <m-array.h>
#include <toolbox/stream/file_stream.h>

#include "scenes/momentum_app_scene.h"

ARRAY_DEF(CharList, char*)
ARRAY_DEF(FrequencyList, uint32_t)

#ifndef MAINMENU_APPS_PATH
#define MAINMENU_APPS_PATH INT_PATH(".mainmenu_apps.txt")
#endif

#define DOLPHIN_MAX_XP 999999

// From dolphin_state.h (not available in ESP32 port)
#define BUTTHURT_MAX 14

extern const uint32_t DOLPHIN_LEVELS[];
extern const size_t DOLPHIN_LEVEL_COUNT;

static inline int rgbcmp(const RgbColor* a, const RgbColor* b) {
    if(a->r != b->r) return 1;
    if(a->g != b->g) return 1;
    if(a->b != b->b) return 1;
    return 0;
}

// RGB backlight stubs (ESP32 port has no RGB backlight hardware)
#define RGBBacklightRainbowModeCount 3
static inline void rgb_backlight_set_color(uint8_t led, const RgbColor* color) {
    (void)led;
    (void)color;
}
static inline void rgb_backlight_get_color(uint8_t led, RgbColor* color) {
    (void)led;
    color->r = 0;
    color->g = 0;
    color->b = 0;
}
static inline void rgb_backlight_set_rainbow_mode(uint8_t mode) {
    (void)mode;
}
static inline uint8_t rgb_backlight_get_rainbow_mode(void) {
    return 0;
}
static inline void rgb_backlight_set_rainbow_speed(uint8_t speed) {
    (void)speed;
}
static inline uint8_t rgb_backlight_get_rainbow_speed(void) {
    return 1;
}
static inline void rgb_backlight_set_rainbow_interval(uint32_t interval) {
    (void)interval;
}
static inline uint32_t rgb_backlight_get_rainbow_interval(void) {
    return 1000;
}
static inline void rgb_backlight_set_rainbow_saturation(uint8_t saturation) {
    (void)saturation;
}
static inline uint8_t rgb_backlight_get_rainbow_saturation(void) {
    return 255;
}
static inline void rgb_backlight_reconfigure(bool enable) {
    (void)enable;
}

typedef struct {
    Gui* gui;
    Storage* storage;
    Desktop* desktop;
    Dolphin* dolphin;
    DialogsApp* dialogs;
    NotificationApp* notification;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    VariableItemList* var_item_list;
    Submenu* submenu;
    TextInput* text_input;
    ByteInput* byte_input;
    NumberInput* number_input;
    Popup* popup;
    DialogEx* dialog_ex;

    CharList_t asset_pack_names;
    uint8_t asset_pack_index;
    CharList_t mainmenu_app_labels;
    CharList_t mainmenu_app_exes;
    uint8_t mainmenu_app_index;
    DesktopSettings desktop_settings;
    bool subghz_use_defaults;
    FrequencyList_t subghz_static_freqs;
    uint8_t subghz_static_index;
    FrequencyList_t subghz_hopper_freqs;
    uint8_t subghz_hopper_index;
    bool subghz_extend;
    bool subghz_bypass;
    RgbColor lcd_color;
    RgbColor vgm_color;
    char device_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    uint32_t dolphin_xp;
    uint32_t dolphin_angry;
    FuriString* version_tag;

    bool save_mainmenu_apps;
    bool save_desktop;
    bool save_subghz_freqs;
    bool save_subghz;
    bool save_name;
    bool save_xp;
    bool save_angry;
    bool save_dolphin;
    bool save_backlight;
    bool save_settings;
    bool apply_pack;
    bool show_slideshow;
    bool require_reboot;
} MomentumApp;

typedef enum {
    MomentumAppViewVarItemList,
    MomentumAppViewSubmenu,
    MomentumAppViewTextInput,
    MomentumAppViewByteInput,
    MomentumAppViewNumberInput,
    MomentumAppViewPopup,
    MomentumAppViewDialogEx,
} MomentumAppView;

bool momentum_app_apply(MomentumApp* app);

void momentum_app_push_mainmenu_app(MomentumApp* app, FuriString* exe);
void momentum_app_load_mainmenu_apps(MomentumApp* app);
void momentum_app_empty_mainmenu_apps(MomentumApp* app);
