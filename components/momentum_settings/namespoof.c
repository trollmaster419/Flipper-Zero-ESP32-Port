#include "namespoof.h"

#include <flipper_format/flipper_format.h>
#include <furi_hal_version.h>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "NameSpoof"

void namespoof_init(void) {
    FuriString* str = furi_string_alloc();
    Storage* storage = furi_record_open(RECORD_STORAGE);

    /* Wait for the SD card to be ready. The name.settings file lives on
     * /ext, so we can't read it before the card is mounted.  Namechanger
     * does the same wait in namechanger_init() — we follow suit. */
    for(int i = 0; i < 12; i++) {
        if(storage_sd_status(storage) == FSE_OK) break;
        furi_delay_ms(250);
    }

    bool name_applied = false;
    FlipperFormat* file = flipper_format_file_alloc(storage);

    do {
        uint32_t version;
        if(!flipper_format_file_open_existing(file, NAMESPOOF_PATH)) break;
        if(!flipper_format_read_header(file, str, &version)) break;
        if(furi_string_cmp_str(str, NAMESPOOF_HEADER)) break;
        if(version != NAMESPOOF_VERSION) break;

        if(!flipper_format_read_string(file, "Name", str)) break;
        furi_hal_version_set_name(furi_string_get_cstr(str));
        name_applied = true;
    } while(false);

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(str);

    /* Fallback: if the SD card read failed, try NVS.  furi_hal_version_init()
     * already does this at boot, but we may be called before it had a chance —
     * re-apply here in case NVS has a saved name. */
    if(!name_applied) {
        nvs_handle_t h;
        char nvs_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
        size_t len = sizeof(nvs_name);
        if(nvs_open("fapcfg", NVS_READONLY, &h) == ESP_OK) {
            if(nvs_get_str(h, "device_name", nvs_name, &len) == ESP_OK) {
                nvs_close(h);
                furi_hal_version_set_name(nvs_name);
            } else {
                nvs_close(h);
            }
        }
    }
}
