idf_component_register(
    SRCS "lfs.c" "lfs_util.c" "esp_littlefs.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition vfs
)
