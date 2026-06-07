#include "momentum_settings.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_resources.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_types.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG "MomentumSettings"

MomentumSettings momentum_settings = {
    .asset_pack = "",
    .anim_speed = 100,
    .cycle_anims = 0,
    .unlock_anims = false,
    .menu_style = MenuStyleDsi,
    .lock_on_boot = true,
    .bad_pins_format = false,
    .allow_locked_rpc_usb = false,
    .allow_locked_rpc_ble = false,
    .lockscreen_poweroff = true,
    .lockscreen_time = true,
    .lockscreen_seconds = false,
    .lockscreen_date = true,
    .lockscreen_statusbar = true,
    .lockscreen_prompt = true,
    .lockscreen_transparent = false,
    .lockscreen_skip_animation = false,
    .battery_icon = BatteryIconBarPercent,
    .status_icons = true,
    .bar_borders = true,
    .bar_background = false,
    .sort_dirs_first = true,
    .show_hidden_files = false,
    .show_internal_tab = false,
    .browser_path_mode = BrowserPathOff,
    .favorite_timeout = 0,
    .scroll_marquee = false,
    .dark_mode = false,
    .rgb_backlight = false,
    .butthurt_timer = 21600,
    .midnight_format_00 = true,
    .popup_overlay = true,
    .spi_cc1101_handle = SpiBruce,
    .spi_nrf24_handle = SpiBruce,
    .uart_esp_channel = UartBruce,
    .uart_nmea_channel = UartBruce,
    .file_naming_prefix_after = false,
    .spoof_color = FuriHalVersionColorUnknown,
    .rpc_color_fg = {{ScreenColorModeDefault, {0, 0, 0}}},
    .rpc_color_bg = {{ScreenColorModeDefault, {255, 130, 0}}},
    .ir_tx_pin = IrTxPinG19,
    .nfc_pins = NfcPinsG26G25,
};

typedef enum {
    momentum_settings_type_str,
    momentum_settings_type_int,
    momentum_settings_type_uint,
    momentum_settings_type_bool,
} momentum_settings_type;

static const struct {
    momentum_settings_type type;
    const char* key;
    void* val;
    union {
        size_t str_len;
        struct {
            int32_t i_min;
            int32_t i_max;
            uint8_t i_sz;
        };
        struct {
            uint32_t u_min;
            uint32_t u_max;
            uint8_t u_sz;
        };
    };
#define setting(t, n)             .type = momentum_settings_type##t, .key = #n, .val = &momentum_settings.n
#define setting_str(n)            setting(_str, n), .str_len = sizeof(momentum_settings.n)
#define num(t, n, min, max)       .t##_min = min, .t##_max = max, .t##_sz = sizeof(momentum_settings.n)
#define setting_int(n, min, max)  setting(_int, n), num(i, n, min, max)
#define setting_uint(n, min, max) setting(_uint, n), num(u, n, min, max)
#define setting_enum(n, cnt)      setting_uint(n, 0, cnt - 1)
#define setting_bool(n)           setting(_bool, n)
} momentum_settings_entries[] = {
    {setting_str(asset_pack)},
    {setting_uint(anim_speed, 25, 300)},
    {setting_int(cycle_anims, -1, 86400)},
    {setting_bool(unlock_anims)},
    {setting_enum(menu_style, MenuStyleCount)},
    {setting_bool(bad_pins_format)},
    {setting_bool(allow_locked_rpc_usb)},
    {setting_bool(allow_locked_rpc_ble)},
    {setting_bool(lock_on_boot)},
    {setting_bool(lockscreen_poweroff)},
    {setting_bool(lockscreen_time)},
    {setting_bool(lockscreen_seconds)},
    {setting_bool(lockscreen_date)},
    {setting_bool(lockscreen_statusbar)},
    {setting_bool(lockscreen_prompt)},
    {setting_bool(lockscreen_transparent)},
    {setting_bool(lockscreen_skip_animation)},
    {setting_enum(battery_icon, BatteryIconCount)},
    {setting_bool(status_icons)},
    {setting_bool(bar_borders)},
    {setting_bool(bar_background)},
    {setting_bool(sort_dirs_first)},
    {setting_bool(show_hidden_files)},
    {setting_bool(show_internal_tab)},
    {setting_enum(browser_path_mode, BrowserPathModeCount)},
    {setting_uint(favorite_timeout, 0, 60)},
    {setting_bool(scroll_marquee)},
    {setting_bool(dark_mode)},
    {setting_bool(rgb_backlight)},
    {setting_uint(butthurt_timer, 0, 172800)},
    {setting_bool(midnight_format_00)},
    {setting_bool(popup_overlay)},
    {setting_enum(spi_cc1101_handle, SpiCount)},
    {setting_enum(spi_nrf24_handle, SpiCount)},
    {setting_enum(uart_esp_channel, UartCount)},
    {setting_enum(uart_nmea_channel, UartCount)},
    {setting_bool(file_naming_prefix_after)},
    {setting_enum(spoof_color, FuriHalVersionColorCount)},
    {setting_uint(rpc_color_fg, 0x000000, 0xFFFFFF)},
    {setting_uint(rpc_color_bg, 0x000000, 0xFFFFFF)},
    {setting_enum(ir_tx_pin, IrTxPinCount)},
    {setting_enum(nfc_pins, NfcPinsCount)},
};

static const GpioPin* spi_pins_mosi[] = {
    [SpiDisabled] = &gpio_null,
    [SpiBruce] = &gpio_spi_bruce_mosi,
    [SpiDefault] = &gpio_ext_pa7,
    [SpiExtra] = &gpio_ext_pa7,
};
static const GpioPin* spi_pins_miso[] = {
    [SpiDisabled] = &gpio_null,
    [SpiBruce] = &gpio_spi_bruce_miso,
    [SpiDefault] = &gpio_ext_pa6,
    [SpiExtra] = &gpio_ext_pa6,
};
static const GpioPin* spi_pins_sck[] = {
    [SpiDisabled] = &gpio_null,
    [SpiBruce] = &gpio_spi_bruce_sck,
    [SpiDefault] = &gpio_ext_pb3,
    [SpiExtra] = &gpio_ext_pb3,
};
static const GpioPin* spi_pins_cs_cc1101[] = {
    [SpiDisabled] = &gpio_null,
    [SpiBruce] = &gpio_spi_bruce_cs,
    [SpiDefault] = &gpio_ext_pa4,
    [SpiExtra] = &gpio_ext_pa4,
};
static const GpioPin* spi_pins_cs_nrf24[] = {
    [SpiDisabled] = &gpio_null,
    [SpiBruce] = &gpio_spi_bruce_cs,
    [SpiDefault] = &gpio_nrf24_cs,
    [SpiExtra] = &gpio_nrf24_cs,
};

static void momentum_settings_apply_spi_config(void) {
    /* De-init handles so next furi_hal_spi_acquire re-inits with new pins */
    furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_subghz);
    furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_nrf24);
    furi_hal_spi_bus_subghz.initialized = false;

    SpiHandle cc1101 = momentum_settings.spi_cc1101_handle;
    if(cc1101 >= SpiCount) cc1101 = SpiBruce;
    furi_hal_spi_bus_handle_subghz.miso = spi_pins_miso[cc1101];
    furi_hal_spi_bus_handle_subghz.mosi = spi_pins_mosi[cc1101];
    furi_hal_spi_bus_handle_subghz.sck = spi_pins_sck[cc1101];
    furi_hal_spi_bus_handle_subghz.cs = spi_pins_cs_cc1101[cc1101];

    SpiHandle nrf24 = momentum_settings.spi_nrf24_handle;
    if(nrf24 >= SpiCount) nrf24 = SpiBruce;
    furi_hal_spi_bus_handle_nrf24.miso = spi_pins_miso[nrf24];
    furi_hal_spi_bus_handle_nrf24.mosi = spi_pins_mosi[nrf24];
    furi_hal_spi_bus_handle_nrf24.sck = spi_pins_sck[nrf24];
    furi_hal_spi_bus_handle_nrf24.cs = spi_pins_cs_nrf24[nrf24];
}

void momentum_settings_load(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_existing(file, MOMENTUM_SETTINGS_PATH)) {
        FuriString* val_str = furi_string_alloc();
        int32_t val_int;
        uint32_t val_uint;
        bool val_bool;

        bool ok;
        for(size_t entry_i = 0; entry_i < COUNT_OF(momentum_settings_entries); entry_i++) {
            switch(momentum_settings_entries[entry_i].type) {
            case momentum_settings_type_str:
                ok = flipper_format_read_string(
                    file, momentum_settings_entries[entry_i].key, val_str);
                if(ok)
                    strlcpy(
                        (char*)momentum_settings_entries[entry_i].val,
                        furi_string_get_cstr(val_str),
                        momentum_settings_entries[entry_i].str_len);
                break;
            case momentum_settings_type_int:
                ok = flipper_format_read_int32(
                    file, momentum_settings_entries[entry_i].key, &val_int, 1);
                val_int = CLAMP(
                    val_int,
                    momentum_settings_entries[entry_i].i_max,
                    momentum_settings_entries[entry_i].i_min);
                if(ok) memcpy(momentum_settings_entries[entry_i].val, &val_int, momentum_settings_entries[entry_i].i_sz);
                break;
            case momentum_settings_type_uint:
                ok = flipper_format_read_uint32(
                    file, momentum_settings_entries[entry_i].key, &val_uint, 1);
                val_uint = CLAMP(
                    val_uint,
                    momentum_settings_entries[entry_i].u_max,
                    momentum_settings_entries[entry_i].u_min);
                if(ok) memcpy(momentum_settings_entries[entry_i].val, &val_uint, momentum_settings_entries[entry_i].u_sz);
                break;
            case momentum_settings_type_bool:
                ok = flipper_format_read_bool(
                    file, momentum_settings_entries[entry_i].key, &val_bool, 1);
                if(ok) *(bool*)momentum_settings_entries[entry_i].val = val_bool;
                break;
            default:
                continue;
            }
            if(!ok) flipper_format_rewind(file);
        }

        furi_string_free(val_str);
    }
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    /* Save IR TX pin to NVS and apply immediately */
    furi_hal_infrared_save_tx_output(
        momentum_settings.ir_tx_pin == IrTxPinG19 ? FuriHalInfraredTxPinInternal :
                                                     FuriHalInfraredTxPinExtPA7);

    /* Save NFC pin config to NVS and de-init if disabled */
    furi_hal_nfc_set_pins_config(momentum_settings.nfc_pins);

    /* Apply SPI pin config (Disabled/Bruce/Default/Extra) */
    momentum_settings_apply_spi_config();
}

void momentum_settings_save(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);

    if(flipper_format_file_open_always(file, MOMENTUM_SETTINGS_PATH)) {
        int32_t tmp_int;
        uint32_t tmp_uint;
        for(size_t entry_i = 0; entry_i < COUNT_OF(momentum_settings_entries); entry_i++) {
            switch(momentum_settings_entries[entry_i].type) {
            case momentum_settings_type_str:
                flipper_format_write_string_cstr(
                    file,
                    momentum_settings_entries[entry_i].key,
                    (char*)momentum_settings_entries[entry_i].val);
                break;
            case momentum_settings_type_int:
                tmp_int = 0;
                memcpy(&tmp_int, momentum_settings_entries[entry_i].val, momentum_settings_entries[entry_i].i_sz);
                flipper_format_write_int32(
                    file, momentum_settings_entries[entry_i].key, &tmp_int, 1);
                break;
            case momentum_settings_type_uint:
                tmp_uint = 0;
                memcpy(&tmp_uint, momentum_settings_entries[entry_i].val, momentum_settings_entries[entry_i].u_sz);
                flipper_format_write_uint32(
                    file, momentum_settings_entries[entry_i].key, &tmp_uint, 1);
                break;
            case momentum_settings_type_bool:
                flipper_format_write_bool(
                    file,
                    momentum_settings_entries[entry_i].key,
                    (bool*)momentum_settings_entries[entry_i].val,
                    1);
                break;
            default:
                continue;
            }
        }
    }

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}
