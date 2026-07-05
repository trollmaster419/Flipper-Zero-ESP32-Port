#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t total_size;
    uint64_t used_size;
    uint32_t block_size;
    uint32_t block_count;
} esp_littlefs_info_t;

bool esp_littlefs_mount(const char *partition_label, const char *mount_point);
bool esp_littlefs_unmount(void);
bool esp_littlefs_format(const char *partition_label);
bool esp_littlefs_info(const char *partition_label, esp_littlefs_info_t *info);
bool esp_littlefs_mounted(void);

#ifdef __cplusplus
}
#endif
