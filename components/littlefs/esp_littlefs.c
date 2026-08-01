#include "esp_littlefs.h"
#include "lfs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_partition.h>
#include <esp_vfs.h>
#include <esp_log.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "esp_littlefs";

static lfs_t lfs_handle;
static lfs_t *lfs_ptr = NULL;
static const esp_partition_t *lfs_partition = NULL;
static char lfs_mount_point[64];
static bool lfs_mounted = false;

// ESP-IDF's VFS layer stores each open file's local fd in a uint8_t
// (typedef uint8_t local_fd_t in vfs.c). The value returned from vfs_open()
// must therefore be a small index, NOT a pointer: a 32-bit lfs_file_t* gets
// truncated to its low 8 bits and resolves to a bogus near-NULL pointer on the
// next read/write/fstat (crash at EXCVADDR ~= field offset). Map small fd
// indices to file pointers through this table instead.
#define LFS_MAX_OPEN_FILES 16
static lfs_file_t *lfs_open_files[LFS_MAX_OPEN_FILES];
static portMUX_TYPE lfs_fd_mux = portMUX_INITIALIZER_UNLOCKED;

// littlefs is single-threaded by design: every lfs_* call mutates shared state
// (the open-file mlist, block caches, metadata). The storage service serializes
// its own callers, but other threads reach this VFS directly (POSIX fopen/stat,
// a second RPC session, etc.). Without a lock here, two concurrent FS users
// corrupt the mlist — e.g. a LoadProhibited in lfs_mlist_isopen during a mobile
// app sync racing another task. Serialize all lfs_* access at the VFS layer.
static SemaphoreHandle_t lfs_op_mux = NULL;

static inline void lfs_op_lock(void) {
    if (lfs_op_mux) xSemaphoreTake(lfs_op_mux, portMAX_DELAY);
}
static inline void lfs_op_unlock(void) {
    if (lfs_op_mux) xSemaphoreGive(lfs_op_mux);
}

/* Map an LFS error to a POSIX errno. The ESP-IDF VFS layer (CHECK_AND_CALL in
 * vfs.c) passes our return value through verbatim and does NOT set errno, so
 * every error path here must `errno = lfs_to_errno(err); return -1;`. Returning
 * a raw -Exxx instead leaves errno stale, which silently breaks callers that
 * inspect errno after a failed stat()/open() (e.g. storage_common_stat treating
 * a missing file as FSE_OK because errno happened to be 0). */
static int lfs_to_errno(int lfs_err) {
    switch (lfs_err) {
    case LFS_ERR_OK:          return 0;
    case LFS_ERR_NOENT:       return ENOENT;
    case LFS_ERR_EXIST:       return EEXIST;
    case LFS_ERR_NOTDIR:      return ENOTDIR;
    case LFS_ERR_ISDIR:       return EISDIR;
    case LFS_ERR_NOTEMPTY:    return ENOTEMPTY;
    case LFS_ERR_NOSPC:       return ENOSPC;
    case LFS_ERR_NOMEM:       return ENOMEM;
    case LFS_ERR_INVAL:       return EINVAL;
    case LFS_ERR_NAMETOOLONG: return ENAMETOOLONG;
    default:                  return EIO;
    }
}

static lfs_file_t *lfs_file_from_fd(int fd) {
    if (fd < 0 || fd >= LFS_MAX_OPEN_FILES) return NULL;
    return lfs_open_files[fd];
}

static int lfs_esp_read(const struct lfs_config *cfg, lfs_block_t block,
                        lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)cfg;
    esp_err_t err = esp_partition_read(lfs_partition,
                                       (size_t)block * lfs_partition->erase_size + off,
                                       buffer, size);
    return err == ESP_OK ? 0 : LFS_ERR_IO;
}

static int lfs_esp_prog(const struct lfs_config *cfg, lfs_block_t block,
                        lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)cfg;
    esp_err_t err = esp_partition_write(lfs_partition,
                                        (size_t)block * lfs_partition->erase_size + off,
                                        buffer, size);
    return err == ESP_OK ? 0 : LFS_ERR_IO;
}

static int lfs_esp_erase(const struct lfs_config *cfg, lfs_block_t block) {
    (void)cfg;
    esp_err_t err = esp_partition_erase_range(lfs_partition,
                                              (size_t)block * lfs_partition->erase_size,
                                              lfs_partition->erase_size);
    return err == ESP_OK ? 0 : LFS_ERR_IO;
}

static int lfs_esp_sync(const struct lfs_config *cfg) {
    (void)cfg;
    return 0;
}

static struct lfs_config lfs_cfg;

// Fill an lfs_config for the given partition. cache_size must be a multiple of
// read_size/prog_size and a factor of block_size; lookahead_size a multiple of 8.
static void fill_lfs_config(struct lfs_config *cfg, const esp_partition_t *part) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->read = lfs_esp_read;
    cfg->prog = lfs_esp_prog;
    cfg->erase = lfs_esp_erase;
    cfg->sync = lfs_esp_sync;
    cfg->read_size = 128;
    cfg->prog_size = 128;
    cfg->block_size = part->erase_size;
    cfg->block_count = part->size / part->erase_size;
    cfg->block_cycles = 500;
    cfg->cache_size = 512;
    cfg->lookahead_size = 128;
}

static esp_err_t init_lfs_config(void) {
    if (!lfs_partition) {
        return ESP_ERR_NOT_FOUND;
    }
    fill_lfs_config(&lfs_cfg, lfs_partition);
    return ESP_OK;
}

/* --- VFS glue --- */

static int vfs_open(void *ctx, const char *path, int flags, int mode) {
    (void)ctx;
    (void)mode;
    if (!lfs_ptr) { errno = ENODEV; return -1; }

    lfs_file_t *file = malloc(sizeof(lfs_file_t));
    if (!file) { errno = ENOMEM; return -1; }

    int lfs_flags = 0;
    int acc_mode = flags & O_ACCMODE;
    if (acc_mode == O_RDONLY) lfs_flags = LFS_O_RDONLY;
    else if (acc_mode == O_WRONLY) lfs_flags = LFS_O_WRONLY;
    else if (acc_mode == O_RDWR) lfs_flags = LFS_O_RDWR;

    if (flags & O_CREAT) lfs_flags |= LFS_O_CREAT;
    if (flags & O_TRUNC) lfs_flags |= LFS_O_TRUNC;
    if (flags & O_APPEND) lfs_flags |= LFS_O_APPEND;
    if (flags & O_EXCL) lfs_flags |= LFS_O_EXCL;

    lfs_op_lock();
    int err = lfs_file_open(lfs_ptr, file, path, lfs_flags);
    lfs_op_unlock();
    if (err) {
        free(file);
        errno = lfs_to_errno(err);
        return -1;
    }

    // Allocate a small fd index (see lfs_open_files comment above).
    int slot = -1;
    portENTER_CRITICAL(&lfs_fd_mux);
    for (int i = 0; i < LFS_MAX_OPEN_FILES; i++) {
        if (!lfs_open_files[i]) {
            lfs_open_files[i] = file;
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&lfs_fd_mux);

    if (slot < 0) {
        lfs_op_lock();
        lfs_file_close(lfs_ptr, file);
        lfs_op_unlock();
        free(file);
        errno = ENFILE;
        return -1;
    }
    return slot;
}

static ssize_t vfs_write(void *ctx, int fd, const void *data, size_t size) {
    (void)ctx;
    lfs_file_t *file = lfs_file_from_fd(fd);
    if (!file) { errno = EBADF; return -1; }
    lfs_op_lock();
    lfs_ssize_t ret = lfs_file_write(lfs_ptr, file, data, (lfs_size_t)size);
    lfs_op_unlock();
    if (ret < 0) { errno = lfs_to_errno(ret); return -1; }
    return (ssize_t)ret;
}

static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size) {
    (void)ctx;
    lfs_file_t *file = lfs_file_from_fd(fd);
    if (!file) { errno = EBADF; return -1; }
    lfs_op_lock();
    lfs_ssize_t ret = lfs_file_read(lfs_ptr, file, dst, (lfs_size_t)size);
    lfs_op_unlock();
    if (ret < 0) { errno = lfs_to_errno(ret); return -1; }
    return (ssize_t)ret;
}

static int vfs_close(void *ctx, int fd) {
    (void)ctx;
    lfs_file_t *file = lfs_file_from_fd(fd);
    if (!file) { errno = EBADF; return -1; }
    portENTER_CRITICAL(&lfs_fd_mux);
    lfs_open_files[fd] = NULL;
    portEXIT_CRITICAL(&lfs_fd_mux);
    lfs_op_lock();
    int err = lfs_file_close(lfs_ptr, file);
    lfs_op_unlock();
    free(file);
    if (err) { errno = lfs_to_errno(err); return -1; }
    return 0;
}

static off_t vfs_lseek(void *ctx, int fd, off_t offset, int whence) {
    (void)ctx;
    lfs_file_t *file = lfs_file_from_fd(fd);
    if (!file) { errno = EBADF; return -1; }
    int lfs_whence;
    if (whence == SEEK_SET) lfs_whence = LFS_SEEK_SET;
    else if (whence == SEEK_CUR) lfs_whence = LFS_SEEK_CUR;
    else if (whence == SEEK_END) lfs_whence = LFS_SEEK_END;
    else { errno = EINVAL; return -1; }

    lfs_op_lock();
    lfs_soff_t ret = lfs_file_seek(lfs_ptr, file, (lfs_off_t)offset, lfs_whence);
    lfs_op_unlock();
    if (ret < 0) { errno = lfs_to_errno(ret); return -1; }
    return (off_t)ret;
}

static int vfs_fstat(void *ctx, int fd, struct stat *st) {
    (void)ctx;
    lfs_file_t *file = lfs_file_from_fd(fd);
    if (!file) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(struct stat));
    lfs_op_lock();
    st->st_size = (off_t)lfs_file_size(lfs_ptr, file);
    lfs_op_unlock();
    st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    return 0;
}

static int vfs_fsync(void *ctx, int fd) {
    (void)ctx;
    lfs_file_t *file = lfs_file_from_fd(fd);
    if (!file) { errno = EBADF; return -1; }
    lfs_op_lock();
    int err = lfs_file_sync(lfs_ptr, file);
    lfs_op_unlock();
    if (err) { errno = lfs_to_errno(err); return -1; }
    return 0;
}

static int vfs_stat(void *ctx, const char *path, struct stat *st) {
    (void)ctx;
    memset(st, 0, sizeof(struct stat));

    while (*path == '/') path++;
    if (*path == '\0') {
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        return 0;
    }

    struct lfs_info info;
    lfs_op_lock();
    int err = lfs_stat(lfs_ptr, path, &info);
    lfs_op_unlock();
    if (err) {
        errno = lfs_to_errno(err);
        return -1;
    }

    st->st_size = (off_t)info.size;
    if (info.type == LFS_TYPE_DIR) {
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    } else {
        st->st_mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;
    }
    return 0;
}

static int vfs_mkdir(void *ctx, const char *path, mode_t mode) {
    (void)ctx;
    (void)mode;
    lfs_op_lock();
    int err = lfs_mkdir(lfs_ptr, path);
    lfs_op_unlock();
    if (err) {
        errno = lfs_to_errno(err);
        return -1;
    }
    return 0;
}

static int vfs_unlink(void *ctx, const char *path) {
    (void)ctx;
    lfs_op_lock();
    int err = lfs_remove(lfs_ptr, path);
    lfs_op_unlock();
    if (err) {
        errno = lfs_to_errno(err);
        return -1;
    }
    return 0;
}

static int vfs_rename(void *ctx, const char *old_path, const char *new_path) {
    (void)ctx;
    lfs_op_lock();
    int err = lfs_rename(lfs_ptr, old_path, new_path);
    lfs_op_unlock();
    if (err) {
        errno = lfs_to_errno(err);
        return -1;
    }
    return 0;
}

/* ESP-IDF VFS writes dd_vfs_idx at offset 0 of the returned DIR*,
   which would corrupt lfs_dir_t.mlist.next. Absorb it with a wrapper. */
typedef struct {
    int dd_vfs_idx;
    lfs_dir_t dir;
} lfs_dir_wrapper_t;

#define DIR_TO_LFS(pdir)  (&((lfs_dir_wrapper_t *)(pdir))->dir)

static DIR *vfs_opendir(void *ctx, const char *path) {
    (void)ctx;
    lfs_dir_wrapper_t *wrap = malloc(sizeof(lfs_dir_wrapper_t));
    if (!wrap) {
        errno = ENOMEM;
        return NULL;
    }

    lfs_op_lock();
    int err = lfs_dir_open(lfs_ptr, &wrap->dir, path);
    lfs_op_unlock();
    if (err) {
        free(wrap);
        errno = lfs_to_errno(err);
        return NULL;
    }

    return (DIR *)wrap;
}

static int vfs_closedir(void *ctx, DIR *pdir) {
    (void)ctx;
    lfs_dir_wrapper_t *wrap = (lfs_dir_wrapper_t *)pdir;
    lfs_op_lock();
    int err = lfs_dir_close(lfs_ptr, &wrap->dir);
    lfs_op_unlock();
    free(wrap);
    if (err) { errno = lfs_to_errno(err); return -1; }
    return 0;
}

static struct dirent *vfs_readdir(void *ctx, DIR *pdir) {
    (void)ctx;
    lfs_dir_t *dir = DIR_TO_LFS(pdir);

    static struct lfs_info info;
    static struct dirent entry;
    memset(&entry, 0, sizeof(entry));

    lfs_op_lock();
    int ret = lfs_dir_read(lfs_ptr, dir, &info);
    if (ret > 0) {
        /* Copy the name while still holding the lock: `info` is a static
         * buffer shared by every readdir, so a concurrent iteration of a
         * different directory can overwrite it the moment we unlock. */
        entry.d_ino = 0;
        entry.d_type = (info.type == LFS_TYPE_DIR) ? DT_DIR : DT_REG;
        strncpy(entry.d_name, info.name, sizeof(entry.d_name) - 1);
        entry.d_name[sizeof(entry.d_name) - 1] = '\0';
    }
    lfs_op_unlock();
    if (ret <= 0) return NULL;
    return &entry;
}

static int vfs_readdir_r(void *ctx, DIR *pdir, struct dirent *entry,
                         struct dirent **out_entry) {
    (void)ctx;
    (void)entry;
    lfs_dir_t *dir = DIR_TO_LFS(pdir);

    static struct lfs_info info;
    lfs_op_lock();
    int ret = lfs_dir_read(lfs_ptr, dir, &info);
    if (ret > 0) {
        /* Copy the name while still holding the lock (see vfs_readdir). */
        memset(entry, 0, sizeof(struct dirent));
        entry->d_ino = 0;
        entry->d_type = (info.type == LFS_TYPE_DIR) ? DT_DIR : DT_REG;
        strncpy(entry->d_name, info.name, sizeof(entry->d_name) - 1);
        entry->d_name[sizeof(entry->d_name) - 1] = '\0';
    }
    lfs_op_unlock();
    if (ret <= 0) {
        *out_entry = NULL;
        if (ret < 0) { errno = lfs_to_errno(ret); return -1; }
        return 0;
    }

    *out_entry = entry;
    return 0;
}

static void vfs_seekdir(void *ctx, DIR *pdir, long offset) {
    (void)ctx;
    lfs_dir_t *dir = DIR_TO_LFS(pdir);
    lfs_op_lock();
    lfs_dir_seek(lfs_ptr, dir, (lfs_off_t)offset);
    lfs_op_unlock();
}

static long vfs_telldir(void *ctx, DIR *pdir) {
    (void)ctx;
    lfs_dir_t *dir = DIR_TO_LFS(pdir);
    lfs_op_lock();
    long ret = (long)lfs_dir_tell(lfs_ptr, dir);
    lfs_op_unlock();
    return ret;
}

/* --- Public API --- */

bool esp_littlefs_mount(const char *partition_label, const char *mount_point) {
    if (lfs_mounted) {
        ESP_LOGW(TAG, "Already mounted at %s", lfs_mount_point);
        return true;
    }

    if (!lfs_op_mux) {
        lfs_op_mux = xSemaphoreCreateMutex();
        if (!lfs_op_mux) {
            ESP_LOGE(TAG, "Failed to create littlefs mutex");
            return false;
        }
    }

    lfs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                             partition_label);
    if (!lfs_partition) {
        lfs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 (esp_partition_subtype_t)0x42,
                                                 partition_label);
    }
    if (!lfs_partition) {
        ESP_LOGE(TAG, "Partition '%s' not found", partition_label);
        return false;
    }

    ESP_LOGI(TAG, "Found partition '%s' at offset 0x%" PRIx32 ", size 0x%" PRIx32 " (%" PRIu32 " KB)",
             partition_label, lfs_partition->address, lfs_partition->size,
             lfs_partition->size / 1024);

    if (init_lfs_config() != ESP_OK) {
        return false;
    }

    int err = lfs_mount(&lfs_handle, &lfs_cfg);
    if (err) {
        ESP_LOGW(TAG, "Mount failed (%d), formatting...", err);
        err = lfs_format(&lfs_handle, &lfs_cfg);
        if (err) {
            ESP_LOGE(TAG, "Format failed: %d", err);
            lfs_partition = NULL;
            return false;
        }
        err = lfs_mount(&lfs_handle, &lfs_cfg);
        if (err) {
            ESP_LOGE(TAG, "Re-mount after format failed: %d", err);
            lfs_partition = NULL;
            return false;
        }
    }

    strncpy(lfs_mount_point, mount_point, sizeof(lfs_mount_point) - 1);
    lfs_mount_point[sizeof(lfs_mount_point) - 1] = '\0';

    esp_vfs_t vfs_wrapper = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .write_p = vfs_write,
        .lseek_p = vfs_lseek,
        .read_p = vfs_read,
        .open_p = vfs_open,
        .close_p = vfs_close,
        .fstat_p = vfs_fstat,
        .fsync_p = vfs_fsync,
        .stat_p = vfs_stat,
        .mkdir_p = vfs_mkdir,
        .unlink_p = vfs_unlink,
        .rename_p = vfs_rename,
        .opendir_p = vfs_opendir,
        .closedir_p = vfs_closedir,
        .readdir_p = vfs_readdir,
        .readdir_r_p = vfs_readdir_r,
        .seekdir_p = vfs_seekdir,
        .telldir_p = vfs_telldir,
    };

    esp_err_t ret = esp_vfs_register(mount_point, &vfs_wrapper, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VFS register at '%s' failed: %s", mount_point, esp_err_to_name(ret));
        lfs_partition = NULL;
        return false;
    }

    lfs_ptr = &lfs_handle;
    lfs_mounted = true;

    ESP_LOGI(TAG, "Mounted LittleFS at '%s' (block_size=%" PRIu32 ", block_count=%" PRIu32 ")",
             mount_point, lfs_cfg.block_size, lfs_cfg.block_count);
    return true;
}

bool esp_littlefs_unmount(void) {
    if (!lfs_mounted) return true;

    esp_vfs_unregister(lfs_mount_point);
    lfs_unmount(&lfs_handle);
    lfs_ptr = NULL;
    lfs_partition = NULL;
    lfs_mounted = false;

    ESP_LOGI(TAG, "Unmounted LittleFS");
    return true;
}

bool esp_littlefs_format(const char *partition_label) {
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                                           partition_label);
    if (!part) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        (esp_partition_subtype_t)0x42,
                                        partition_label);
    }
    if (!part) return false;

    esp_partition_erase_range(part, 0, part->size);

    lfs_t lfs;
    struct lfs_config cfg;
    fill_lfs_config(&cfg, part);

    int err = lfs_format(&lfs, &cfg);
    return err == 0;
}

bool esp_littlefs_info(const char *partition_label, esp_littlefs_info_t *info) {
    if (!info) return false;

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                                           partition_label);
    if (!part) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        (esp_partition_subtype_t)0x42,
                                        partition_label);
    }
    if (!part) return false;

    memset(info, 0, sizeof(*info));
    info->total_size = part->size;
    info->block_size = part->erase_size;
    info->block_count = part->size / part->erase_size;

    if (lfs_mounted && lfs_ptr) {
        info->used_size = (uint64_t)lfs_fs_size(lfs_ptr) * part->erase_size;
    }

    return true;
}

bool esp_littlefs_mounted(void) {
    return lfs_mounted;
}
