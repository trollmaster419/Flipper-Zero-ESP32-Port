#include "wlan_sd_update.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>

#define SD_UPDATE_TAG "WlanSdUpdate"

// Raw GitHub content base for downloading files.
#define SD_UPDATE_RAW_BASE "https://raw.githubusercontent.com/trollmaster419/m5resources/main"
#define SD_UPDATE_VERSION_URL SD_UPDATE_RAW_BASE "/version.txt"

// Plain-text manifest hosted alongside the resources, one file per line:
//   V:<n> / T:<unix>          format version / timestamp (ignored)
//   D:<dir>                   directory (ignored — created on demand)
//   F:<md5>:<size>:<path>     a file
// Listing files via the GitHub tree API instead hits the unauthenticated
// 60-req/hr rate limit (HTTP 403) and needs JSON parsing; the raw manifest on
// raw.githubusercontent.com has no rate limit, no User-Agent requirement, and
// no JSON dependency.
#define SD_UPDATE_MANIFEST_URL SD_UPDATE_RAW_BASE "/Manifest"

#define SD_UPDATE_LOCAL_VERSION "/ext/version.txt"
#define SD_UPDATE_DEST_ROOT "/ext"
#define SD_UPDATE_MAX_TREE_JSON (4u * 1024u * 1024u)
#define SD_UPDATE_CHUNK 8192

static void* sd_malloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

struct WlanSdUpdate {
    TaskHandle_t task;
    volatile WlanSdUpdatePhase phase;
    volatile uint8_t percent;
    volatile bool cancel;
    volatile bool running;
    volatile uint32_t speed_kbps;
    volatile uint32_t done_files;
    volatile uint32_t total_files;
    char current_file[64];
    char err[64];
};

static void sd_update_set_file(WlanSdUpdate* u, const char* name) {
    strncpy(u->current_file, name, sizeof(u->current_file) - 1);
    u->current_file[sizeof(u->current_file) - 1] = '\0';
}

static void sd_update_fail(WlanSdUpdate* u, const char* msg) {
    strncpy(u->err, msg, sizeof(u->err) - 1);
    u->err[sizeof(u->err) - 1] = '\0';
    u->phase = WlanSdUpdateError;
    FURI_LOG_E(SD_UPDATE_TAG, "%s", msg);
}

// Trim leading/trailing whitespace (including \r\n) in-place.
static void sd_update_trim(char* s) {
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                    s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t start = 0;
    while(s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n') {
        start++;
    }
    if(start) memmove(s, s + start, strlen(s + start) + 1);
}

static void sd_update_http_cfg(esp_http_client_config_t* cfg, const char* url) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    // api.github.com returns HTTP 403 for any request without a User-Agent.
    cfg->user_agent = "m5stick-sd-update";
    cfg->timeout_ms = 20000;
    cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg->skip_cert_common_name_check = true;
    cfg->crt_bundle_attach = NULL;
    cfg->use_global_ca_store = false;
    cfg->buffer_size = SD_UPDATE_CHUNK;
    cfg->buffer_size_tx = 1024;
    cfg->keep_alive_enable = true;
}

// Download a small text resource synchronously into out (nul-terminated).
static bool sd_update_http_get_text(const char* url, char* out, size_t out_sz) {
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return false;

    bool ok = false;
    if(esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client) == 200) {
            size_t len = 0;
            while(len + 1 < out_sz) {
                int r = esp_http_client_read(client, out + len, out_sz - 1 - len);
                if(r < 0) {
                    len = 0;
                    break;
                }
                if(r == 0) break;
                len += (size_t)r;
            }
            out[len] = '\0';
            ok = len > 0;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

// Download a (potentially large) text resource into a malloc buffer. Caller frees.
static char* sd_update_http_get_alloc(const char* url, size_t* out_len) {
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return NULL;

    char* buf = NULL;
    size_t cap = 0, len = 0;
    bool ok = false;

    if(esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if(status != 200) {
            FURI_LOG_E(SD_UPDATE_TAG, "tree fetch HTTP %d", status);
        }
        if(status == 200) {
            ok = true;
            while(true) {
                if(len + 1 >= cap) {
                    size_t ncap = cap ? cap * 2 : 32768;
                    if(ncap > SD_UPDATE_MAX_TREE_JSON) {
                        ok = false;
                        break;
                    }
                    char* nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
                    if(!nb) {
                        ok = false;
                        break;
                    }
                    buf = nb;
                    cap = ncap;
                }
                int r = esp_http_client_read(client, buf + len, cap - 1 - len);
                if(r < 0) {
                    ok = false;
                    break;
                }
                if(r == 0) break;
                len += (size_t)r;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if(!ok || len == 0) {
        if(buf) free(buf);
        return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static bool sd_update_read_local_version(char* out, size_t out_sz) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    bool ok = false;
    if(storage_file_open(f, SD_UPDATE_LOCAL_VERSION, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t r = storage_file_read(f, out, out_sz - 1);
        out[r] = '\0';
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// true -> local version.txt exists and matches remote version.
static bool sd_update_is_up_to_date(void) {
    char remote[64];
    char local[64];
    if(!sd_update_http_get_text(SD_UPDATE_VERSION_URL, remote, sizeof(remote))) {
        return false;
    }
    if(!sd_update_read_local_version(local, sizeof(local))) {
        return false;
    }
    sd_update_trim(remote);
    sd_update_trim(local);
    return remote[0] != '\0' && strcmp(remote, local) == 0;
}

// Recursively create directories for a path (without the final filename).
static void sd_update_mkdirs(Storage* storage, const char* path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for(char* p = tmp + 1; *p; ++p) {
        if(*p == '/') {
            *p = '\0';
            storage_common_mkdir(storage, tmp);
            *p = '/';
        }
    }
}

// Download a single file via a (reusable) HTTP client to dest on SD.
static bool sd_update_download_file(
    WlanSdUpdate* u,
    esp_http_client_handle_t client,
    Storage* storage,
    const char* url,
    const char* dest) {
    if(esp_http_client_set_url(client, url) != ESP_OK) return false;
    if(esp_http_client_open(client, 0) != ESP_OK) return false;

    bool ok = false;
    File* f = NULL;
    uint8_t* chunk = NULL;

    do {
        esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client) != 200) break;

        sd_update_mkdirs(storage, dest);
        f = storage_file_alloc(storage);
        if(!storage_file_open(f, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) break;

        chunk = malloc(SD_UPDATE_CHUNK);
        if(!chunk) break;

        ok = true;
        uint32_t t0 = furi_get_tick();
        uint32_t total = 0;
        while(!u->cancel) {
            int r = esp_http_client_read(client, (char*)chunk, SD_UPDATE_CHUNK);
            if(r < 0) {
                ok = false;
                break;
            }
            if(r == 0) break;
            if(storage_file_write(f, chunk, (size_t)r) != (size_t)r) {
                ok = false;
                break;
            }
            total += (uint32_t)r;
            uint32_t dt = furi_get_tick() - t0;
            if(dt >= 200) {
                u->speed_kbps = (uint32_t)((uint64_t)total * 1000u / 1024u / dt);
            }
        }
        if(u->cancel) ok = false;
    } while(0);

    if(chunk) free(chunk);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    esp_http_client_close(client);
    return ok;
}

// Path-traversal protection; builds /ext/<rel>.
static bool sd_update_safe_dest(const char* rel, char* out, size_t out_sz) {
    while(*rel == '/') rel++;
    if(!*rel) return false;
    if(strstr(rel, "..")) return false;
    int n = snprintf(out, out_sz, "%s/%s", SD_UPDATE_DEST_ROOT, rel);
    return n > 0 && (size_t)n < out_sz;
}

// Files to skip (repo housekeeping, not SD card resources). Manifest IS
// downloaded to /ext so the device knows which database version is installed.
static bool sd_update_should_skip(const char* path) {
    return strcmp(path, "version.txt") == 0 ||
           strcmp(path, "README.md") == 0 ||
           strcmp(path, ".gitignore") == 0;
}

// A single file entry parsed from the GitHub tree API response.
typedef struct {
    const char* path;
    uint64_t size;
} SdTreeEntry;

// Parse the plain-text manifest into an array of file entries (only 'F:' lines).
// Returns the array (caller frees) and sets *out_count. Returns NULL on error.
// Paths point into the manifest buffer, which is NUL-terminated in place and
// must stay alive while the entries are used.
static SdTreeEntry* sd_update_parse_manifest(char* manifest, uint32_t* out_count) {
    // First pass: count file ('F:') lines.
    uint32_t cap = 0;
    for(char* p = manifest; *p;) {
        if(p[0] == 'F' && p[1] == ':') cap++;
        char* nl = strchr(p, '\n');
        if(!nl) break;
        p = nl + 1;
    }
    if(cap == 0) return NULL;

    SdTreeEntry* entries = sd_malloc(sizeof(SdTreeEntry) * cap);
    if(!entries) return NULL;

    uint32_t n = 0;
    char* p = manifest;
    while(*p && n < cap) {
        char* nl = strchr(p, '\n');
        if(nl) *nl = '\0';

        // F:<md5>:<size>:<path>
        if(p[0] == 'F' && p[1] == ':') {
            char* c1 = strchr(p + 2, ':'); // end of md5
            char* c2 = c1 ? strchr(c1 + 1, ':') : NULL; // end of size
            if(c1 && c2) {
                *c2 = '\0';
                char* size_str = c1 + 1;
                char* path = c2 + 1;
                size_t plen = strlen(path);
                if(plen && path[plen - 1] == '\r') path[--plen] = '\0';
                if(plen && !sd_update_should_skip(path)) {
                    entries[n].path = path;
                    entries[n].size = strtoull(size_str, NULL, 10);
                    n++;
                }
            }
        }

        if(!nl) break;
        p = nl + 1;
    }

    *out_count = n;
    return entries;
}

// Main sync: fetch the tree, compare with local files, download what's needed.
static bool sd_update_sync(WlanSdUpdate* u) {
    // Fetch the plain-text manifest (lists every file + size).
    sd_update_set_file(u, "file list");
    size_t manifest_len = 0;
    char* manifest = sd_update_http_get_alloc(SD_UPDATE_MANIFEST_URL, &manifest_len);
    if(!manifest) {
        if(!u->cancel) sd_update_fail(u, "Manifest fetch failed");
        return false;
    }

    // Paths in entries[] point into manifest; it must outlive the download loop.
    uint32_t count = 0;
    SdTreeEntry* entries = sd_update_parse_manifest(manifest, &count);
    if(!entries || count == 0) {
        free(manifest);
        sd_update_fail(u, "No files in manifest");
        return false;
    }

    u->total_files = count;
    u->done_files = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SD_UPDATE_DEST_ROOT);

    // Create a single HTTP client for all downloads (TLS session reuse).
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, SD_UPDATE_RAW_BASE "/version.txt");
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = (client != NULL);

    for(uint32_t i = 0; ok && i < count; i++) {
        if(u->cancel) {
            ok = false;
            break;
        }

        const char* rel = entries[i].path;
        uint64_t want_size = entries[i].size;

        u->done_files = i + 1;
        u->percent = (uint8_t)((uint64_t)(i + 1) * 100u / count);

        char dest[256];
        if(!sd_update_safe_dest(rel, dest, sizeof(dest))) continue;

        // Skip files that already exist with matching size.
        FileInfo fi;
        if(storage_common_stat(storage, dest, &fi) == FSE_OK &&
           want_size > 0 && fi.size == want_size) {
            continue;
        }

        sd_update_set_file(u, rel);
        u->speed_kbps = 0;

        char url[400];
        snprintf(url, sizeof(url), "%s/%s", SD_UPDATE_RAW_BASE, rel);
        FURI_LOG_I(SD_UPDATE_TAG, "fetch %s", rel);
        if(!sd_update_download_file(u, client, storage, url, dest)) {
            if(u->cancel) {
                ok = false;
                break;
            }
            char m[64];
            snprintf(m, sizeof(m), "Download failed: %.32s", rel);
            sd_update_fail(u, m);
            ok = false;
            break;
        }
    }

    if(client) esp_http_client_cleanup(client);
    free(entries);
    free(manifest);

    // Save remote version.txt locally on success.
    if(ok && !u->cancel) {
        char ver[64];
        if(sd_update_http_get_text(SD_UPDATE_VERSION_URL, ver, sizeof(ver))) {
            File* vf = storage_file_alloc(storage);
            if(storage_file_open(vf, SD_UPDATE_LOCAL_VERSION, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                storage_file_write(vf, ver, strlen(ver));
                storage_file_close(vf);
            }
            storage_file_free(vf);
        }
    }

    furi_record_close(RECORD_STORAGE);
    return ok;
}

// ---------------------------------------------------------------------------
// Worker Task
// ---------------------------------------------------------------------------

static void sd_update_finish(WlanSdUpdate* u) {
    u->running = false;
    u->task = NULL;
    vTaskDelete(NULL);
}

static void sd_update_task(void* arg) {
    WlanSdUpdate* u = arg;

    u->phase = WlanSdUpdateChecking;
    u->percent = 0;

    if(!u->cancel && sd_update_is_up_to_date()) {
        u->phase = WlanSdUpdateUpToDate;
        sd_update_finish(u);
        return;
    }
    if(u->cancel) {
        u->phase = WlanSdUpdateIdle;
        sd_update_finish(u);
        return;
    }

    u->phase = WlanSdUpdateDownloading;
    u->percent = 0;

    bool ok = sd_update_sync(u);

    if(ok) {
        u->percent = 100;
        u->phase = WlanSdUpdateDone;
    } else if(u->cancel && u->phase != WlanSdUpdateError) {
        u->phase = WlanSdUpdateIdle;
    }

    sd_update_finish(u);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

WlanSdUpdate* wlan_sd_update_alloc(void) {
    WlanSdUpdate* u = malloc(sizeof(WlanSdUpdate));
    u->task = NULL;
    u->phase = WlanSdUpdateIdle;
    u->percent = 0;
    u->cancel = false;
    u->running = false;
    u->speed_kbps = 0;
    u->done_files = 0;
    u->total_files = 0;
    u->err[0] = '\0';
    sd_update_set_file(u, "version.txt");
    return u;
}

void wlan_sd_update_free(WlanSdUpdate* u) {
    if(!u) return;
    wlan_sd_update_cancel(u);
    free(u);
}

void wlan_sd_update_start(WlanSdUpdate* u) {
    if(u->running) return;
    u->cancel = false;
    u->percent = 0;
    u->speed_kbps = 0;
    u->done_files = 0;
    u->total_files = 0;
    u->err[0] = '\0';
    sd_update_set_file(u, "version.txt");
    u->phase = WlanSdUpdateChecking;
    u->running = true;
    static StaticTask_t sd_upd_tcb;
    size_t sd_stack_size = 8192;
    StackType_t* sd_stack = heap_caps_malloc(sd_stack_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!sd_stack) sd_stack = heap_caps_malloc(sd_stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(sd_stack) {
        u->task = xTaskCreateStatic(sd_update_task, "WlanSdUpd", sd_stack_size, u, 4, sd_stack, &sd_upd_tcb);
    }
    if(!u->task) {
        u->running = false;
        u->task = NULL;
        sd_update_fail(u, "Task spawn failed");
    }
}

void wlan_sd_update_cancel(WlanSdUpdate* u) {
    if(!u->running) return;
    u->cancel = true;
    for(int i = 0; i < 200 && u->running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

WlanSdUpdatePhase wlan_sd_update_get_phase(const WlanSdUpdate* u) {
    return u->phase;
}

uint8_t wlan_sd_update_get_percent(const WlanSdUpdate* u) {
    return u->percent;
}

const char* wlan_sd_update_get_error(const WlanSdUpdate* u) {
    return u->err;
}

bool wlan_sd_update_is_running(const WlanSdUpdate* u) {
    return u->running;
}

const char* wlan_sd_update_get_current_file(const WlanSdUpdate* u) {
    return u->current_file;
}

uint32_t wlan_sd_update_get_speed_kbps(const WlanSdUpdate* u) {
    return u->speed_kbps;
}

uint32_t wlan_sd_update_get_done(const WlanSdUpdate* u) {
    return u->done_files;
}

uint32_t wlan_sd_update_get_total(const WlanSdUpdate* u) {
    return u->total_files;
}
