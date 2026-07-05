#include <furi.h>
#include <furi_hal.h>
#include <flipper.h>
#include <applications.h>
#include <flipper_application/elf/elf_file.h>

#include <esp_log.h>
#include <esp_rom_uart.h>
#include <nvs.h>
#include <nvs_flash.h>

/* Early-readable mirror of the BLE on/off setting (the storage-backed BtSettings can't be read
 * this early). bt_settings_save() writes this key. When BLE is OFF we reserve a large contiguous
 * FAP code pool (BLE and a big pool can't coexist -- the pool starves BLE and freezes the GUI). */
static bool app_main_ble_enabled(void) {
    /* NVS isn't initialized this early yet (BLE start does it later); do it now. Safe to call
     * again later -- nvs_flash_init() is idempotent. */
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvs_handle_t h;
    uint8_t enabled = 1; /* default: BLE on -> no pool -> normal device */
    if(nvs_open("fapcfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if(nvs_get_u8(h, "ble_on", &v) == ESP_OK) enabled = v;
        nvs_close(h);
    }
    return enabled != 0;
}

static const char* TAG = "Main";

static void log_internal_registry(
    const char* label,
    const FlipperInternalApplication* apps,
    size_t count) {
    if(count == 0) {
        ESP_LOGI(TAG, "%s: none", label);
        return;
    }

    for(size_t i = 0; i < count; ++i) {
        ESP_LOGI(
            TAG,
            "%s[%u]: appid=%s name=%s stack=%lu",
            label,
            (unsigned)i,
            apps[i].appid ? apps[i].appid : "(null)",
            apps[i].name ? apps[i].name : "(null)",
            (unsigned long)apps[i].stack_size);
    }
}

static void log_external_registry(
    const char* label,
    const FlipperExternalApplication* apps,
    size_t count) {
    if(count == 0) {
        ESP_LOGI(TAG, "%s: none", label);
        return;
    }

    for(size_t i = 0; i < count; ++i) {
        ESP_LOGI(
            TAG,
            "%s[%u]: name=%s launch=%s",
            label,
            (unsigned)i,
            apps[i].name ? apps[i].name : "(null)",
            apps[i].path ? apps[i].path : "(null)");
    }
}

static bool registry_contains_appid(
    const FlipperInternalApplication* apps,
    size_t count,
    const char* appid) {
    for(size_t i = 0; i < count; ++i) {
        if(apps[i].appid && strcmp(apps[i].appid, appid) == 0) {
            return true;
        }
    }

    return false;
}

static bool registry_contains_launch(
    const FlipperExternalApplication* apps,
    size_t count,
    const char* launch) {
    for(size_t i = 0; i < count; ++i) {
        if(apps[i].path && strcmp(apps[i].path, launch) == 0) {
            return true;
        }
    }

    return false;
}

static void log_registry_snapshot(void) {
    ESP_LOGI(
        TAG,
        "Registry counts: services=%u startup=%u apps=%u settings=%u extsettings=%u external=%u internal_external=%u system=%u debug=%u",
        (unsigned)FLIPPER_SERVICES_COUNT,
        (unsigned)FLIPPER_ON_SYSTEM_START_COUNT,
        (unsigned)FLIPPER_APPS_COUNT,
        (unsigned)FLIPPER_SETTINGS_APPS_COUNT,
        (unsigned)FLIPPER_EXTSETTINGS_APPS_COUNT,
        (unsigned)FLIPPER_EXTERNAL_APPS_COUNT,
        (unsigned)FLIPPER_INTERNAL_EXTERNAL_APPS_COUNT,
        (unsigned)FLIPPER_SYSTEM_APPS_COUNT,
        (unsigned)FLIPPER_DEBUG_APPS_COUNT);

    log_internal_registry("service", FLIPPER_SERVICES, FLIPPER_SERVICES_COUNT);
    log_internal_registry("settings", FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT);
    log_external_registry("extsettings", FLIPPER_EXTSETTINGS_APPS, FLIPPER_EXTSETTINGS_APPS_COUNT);

    if(!registry_contains_appid(FLIPPER_SERVICES, FLIPPER_SERVICES_COUNT, "bt")) {
        ESP_LOGW(TAG, "Bluetooth service appid 'bt' is not present in FLIPPER_SERVICES");
    }

    if(!registry_contains_appid(FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT, "bt_settings") &&
       !registry_contains_launch(
           FLIPPER_EXTSETTINGS_APPS, FLIPPER_EXTSETTINGS_APPS_COUNT, "bt_settings")) {
        ESP_LOGW(
            TAG,
            "Bluetooth settings app 'bt_settings' is not present in settings or extsettings registry");
    }
}

static void furi_log_esp_callback(const uint8_t* data, size_t size, void* context) {
    (void)context;
    /* Stay silent while a host client owns the UART (qFlipper/CLI bridge), so the
     * protobuf/CLI stream isn't corrupted. Logs remain available via `log`. */
    if(furi_log_is_console_muted()) return;
    for(size_t i = 0; i < size; ++i) {
        esp_rom_output_putc((char)data[i]);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting Furi Core on ESP32...");

    // ESP-IDF: Scheduler is already running!
    furi_init();

    // Register ESP32 log handler
    FuriLogHandler log_handler = {
        .callback = furi_log_esp_callback,
        .context = NULL,
    };
    furi_log_add_handler(log_handler);

    /* Reserve a contiguous internal-RAM pool for external FAP code NOW, before furi_hal_init/BLE
     * and the services fragment D/IRAM. At runtime the largest free exec block collapses to ~31KB
     * (pure IRAM), so without this, FAPs with >31KB .text (weather ~33KB, protopirate ~84KB) can't
     * allocate. The pool and BLE can't coexist (a pool big enough to matter starves BLE and freezes
     * the GUI), so we only reserve it when BLE is turned OFF. Toggle BLE off + reboot = big-FAP
     * mode; BLE on = normal device + small FAPs only. (No-op off classic ESP32.) */
    if(!app_main_ble_enabled()) {
        /* 48KB is the proven-safe size: a bigger pool (84-96KB) leaves too little contiguous
         * internal RAM and the Apps browser/loader can't allocate (can't even open Apps). So the
         * effective .text ceiling in big-FAP mode is ~48KB -- enough for weather_station (33KB)
         * etc., but NOT protopirate (84KB), which simply doesn't fit alongside a working system. */
        ESP_LOGI(TAG, "BLE disabled -> big-FAP mode: reserving exec pool");
        fap_exec_pool_init(48 * 1024);
    } else {
        ESP_LOGI(TAG, "BLE enabled -> normal mode: no exec pool (small FAPs only)");
    }

    furi_hal_init_early();
    furi_hal_init();
    flipper_init();

    log_registry_snapshot();

    for(size_t i = 0; i < FLIPPER_SERVICES_COUNT; i++) {
        FuriThread* thread = furi_thread_alloc_service(
            FLIPPER_SERVICES[i].name,
            FLIPPER_SERVICES[i].stack_size,
            FLIPPER_SERVICES[i].app,
            NULL);
        furi_thread_set_appid(thread, FLIPPER_SERVICES[i].appid);
        furi_thread_start(thread);

        furi_delay_ms(10);
    }

    for(size_t i = 0; i < FLIPPER_ON_SYSTEM_START_COUNT; i++) {
        FLIPPER_ON_SYSTEM_START[i]();
    }

    ESP_LOGI(TAG, "All services started, entering background...");

    // This blocks forever (thread scrubber)
    furi_background();
}
