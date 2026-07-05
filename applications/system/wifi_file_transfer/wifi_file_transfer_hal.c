#include "wifi_file_transfer_hal.h"

#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <lwip/ip4_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <btshim.h>

#define TAG "WFT"
#define WFT_SSID "Flipper-FileShare"
#define WFT_PASS "flipper123"
#define WFT_BASE_DIR "/ext"

static esp_netif_t* s_ap_netif = NULL;
static httpd_handle_t s_http = NULL;
static volatile bool s_running = false;
static volatile int s_client_count = 0;
static TaskHandle_t s_worker = NULL;
static bool s_bt_was_on = false;

static void ap_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if(base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_client_count++;
        ESP_LOGI(TAG, "STA connected (%d clients)", s_client_count);
    } else if(base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if(s_client_count > 0) s_client_count--;
        ESP_LOGI(TAG, "STA disconnected (%d clients)", s_client_count);
    }
}

static const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if(!ext) return "application/octet-stream";
    if(strcasecmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if(strcasecmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if(strcasecmp(ext, ".js") == 0) return "application/javascript";
    if(strcasecmp(ext, ".png") == 0) return "image/png";
    if(strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if(strcasecmp(ext, ".gif") == 0) return "image/gif";
    if(strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0) return "text/plain; charset=utf-8";
    if(strcasecmp(ext, ".json") == 0) return "application/json";
    if(strcasecmp(ext, ".zip") == 0) return "application/zip";
    if(strcasecmp(ext, ".fap") == 0) return "application/octet-stream";
    return "application/octet-stream";
}

static void html_escape(FuriString* out, const char* s) {
    for(; *s; s++) {
        switch(*s) {
            case '&': furi_string_cat(out, "&amp;"); break;
            case '<': furi_string_cat(out, "&lt;"); break;
            case '>': furi_string_cat(out, "&gt;"); break;
            case '"': furi_string_cat(out, "&quot;"); break;
            default: furi_string_push_back(out, *s);
        }
    }
}

static void append_file_row(FuriString* body, const char* name, bool is_dir, uint64_t size) {
    furi_string_cat_printf(body, "<tr><td class=\"n\">");
    if(is_dir) {
        furi_string_cat_printf(body, "<span class=\"d\">&#128193; </span>");
        html_escape(body, name);
        furi_string_cat_printf(body, "/</td><td class=\"s\">--</td><td class=\"a\"></td></tr>\n");
    } else {
        furi_string_cat_printf(body, "<span class=\"f\">&#128206; </span>");
        html_escape(body, name);
        furi_string_cat_printf(body, "</td><td class=\"s\">");
        if(size < 1024) {
            furi_string_cat_printf(body, "%llu B", size);
        } else if(size < 1024 * 1024) {
            furi_string_cat_printf(body, "%llu KB", size / 1024);
        } else {
            furi_string_cat_printf(body, "%.1f MB", size / (1024.0 * 1024.0));
        }
        furi_string_cat_printf(body, "</td><td class=\"a\">");
        char enc_name[512];
        char* dp = enc_name;
        for(const char* s = name; *s && dp < enc_name + sizeof(enc_name) - 4; s++) {
            *dp++ = (*s == '/') ? '_' : *s;
        }
        *dp = 0;
        furi_string_cat_printf(body,
            "<a class=\"dl\" href=\"/download/%s\">DL</a> "
            "<form class=\"df\" method=\"post\" action=\"/delete/%s\">"
            "<button class=\"del\">X</button></form>",
            enc_name, enc_name);
        furi_string_cat_printf(body, "</td></tr>\n");
    }
}

static esp_err_t handler_root(httpd_req_t* req) {
    FuriString* html = furi_string_alloc();
    furi_string_cat(html,
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Flipper FileShare</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:16px}"
        "h1{color:#e94560;font-size:1.3em;margin-bottom:12px}"
        "table{width:100%;border-collapse:collapse}"
        "th{text-align:left;color:#888;font-size:.8em;padding:6px 4px;border-bottom:1px solid #333}"
        "td{padding:6px 4px;border-bottom:1px solid #222}"
        "td.n{width:auto}"
        "td.s{width:70px;color:#888;font-size:.85em}"
        "td.a{width:80px;text-align:right;white-space:nowrap}"
        ".d{color:#4ecca3}"
        ".f{color:#e94560}"
        ".dl{color:#4ecca3;text-decoration:none;font-size:.85em;margin-right:8px}"
        ".df{display:inline}"
        ".del{background:#e94560;color:#fff;border:none;border-radius:3px;cursor:pointer;padding:2px 8px;font-size:.8em}"
        ".up{background:#16213e;padding:16px;border-radius:8px;margin:16px 0}"
        ".up h2{color:#4ecca3;font-size:1em;margin-bottom:8px}"
        ".up input[type=file]{color:#eee;font-size:.9em}"
        ".up input[type=submit]{background:#e94560;color:#fff;border:none;padding:8px 20px;border-radius:4px;margin-top:8px}"
        ".st{color:#888;font-size:.8em;margin-top:16px;text-align:center}"
        "a{color:#4ecca3}"
        "</style></head><body>"
        "<h1>&#128206; Flipper FileShare</h1>"
        "<form class=\"up\" method=\"post\" action=\"/upload\" enctype=\"multipart/form-data\">"
        "<h2>Upload File</h2>"
        "<input type=\"file\" name=\"file\" required>"
        "<input type=\"submit\" value=\"Upload\">"
        "</form>"
        "<table><tr><th>Name</th><th>Size</th><th></th></tr>\n"
    );

    DIR* dir = opendir(WFT_BASE_DIR);
    if(dir) {
        struct dirent* ent;
        while((ent = readdir(dir)) != NULL) {
            if(ent->d_name[0] == '.') continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", WFT_BASE_DIR, ent->d_name);
            struct stat st;
            bool is_dir = (ent->d_type == DT_DIR);
            uint64_t size = 0;
            if(stat(full, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                size = st.st_size;
            }
            append_file_row(html, ent->d_name, is_dir, size);
        }
        closedir(dir);
    }

    furi_string_cat(html,
        "</table>"
        "<div class=\"st\">Files are stored on the device. "
        "<a href=\"/\">Refresh</a></div>"
        "</body></html>"
    );

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, furi_string_get_cstr(html), furi_string_size(html));
    furi_string_free(html);
    return ESP_OK;
}

static esp_err_t handler_download(httpd_req_t* req) {
    const char* uri = req->uri + 9;
    if(strlen(uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", WFT_BASE_DIR, uri);
    struct stat st;
    if(stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }
    int fd = open(filepath, O_RDONLY);
    if(fd < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open Failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, get_mime_type(filepath));
    char buf[4096];
    ssize_t n;
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        if(httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            close(fd);
            return ESP_FAIL;
        }
    }
    close(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_431(httpd_req_t* req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_delete(httpd_req_t* req) {
    const char* uri = req->uri + 7;
    if(strlen(uri) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No path");
        return ESP_FAIL;
    }
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", WFT_BASE_DIR, uri);
    unlink(filepath);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char* redirect = "<meta http-equiv=\"refresh\" content=\"0;url=/\">";
    httpd_resp_send(req, redirect, strlen(redirect));
    return ESP_OK;
}

static esp_err_t handler_upload(httpd_req_t* req) {
    char content_type[1024] = {0};
    if(httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        ESP_LOGE(TAG, "upload: no Content-Type header");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No Content-Type");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "upload: Content-Type: %s", content_type);

    char filename[256] = "uploaded";
    char* boundary = strstr(content_type, "boundary=");
    if(!boundary) {
        ESP_LOGW(TAG, "upload: no boundary in Content-Type");
    } else {
        char bval[126];
        const char* src = boundary + 9;
        if(*src == '"') src++;
        int bl = 0;
        while(*src && *src != '"' && *src != ' ' && *src != ';' && bl < (int)sizeof(bval) - 1)
            bval[bl++] = *src++;
        bval[bl] = 0;

        char bound_delim[128];
        snprintf(bound_delim, sizeof(bound_delim), "--%s", bval);
        size_t blen = strlen(bound_delim);
        ESP_LOGI(TAG, "upload: boundary='%s' delim='%s'", bval, bound_delim);

        int total = req->content_len;
        int received = 0;
        char buf[1024];
        enum { SEEK_BOUNDARY, IN_HEADERS, IN_DATA } state = SEEK_BOUNDARY;
        int fd = -1;

        while(received < total) {
            int n = httpd_req_recv(req, buf, sizeof(buf));
            if(n <= 0) {
                ESP_LOGW(TAG, "upload: recv returned %d (received %d/%d)", n, received, total);
                break;
            }
            received += n;
            int pos = 0;
            while(pos < n) {
                if(state == SEEK_BOUNDARY) {
                    if(memcmp(buf + pos, bound_delim, blen) == 0) {
                        state = IN_HEADERS;
                        memset(filename, 0, sizeof(filename));
                        pos += blen;
                        if(pos + 2 <= n && memcmp(buf + pos, "\r\n", 2) == 0) pos += 2;
                    } else {
                        pos++;
                    }
                } else if(state == IN_HEADERS) {
                    char* nl = (char*)memchr(buf + pos, '\n', n - pos);
                    if(!nl) { pos = n; break; }
                    int linelen = (nl - (buf + pos)) + 1;
                    if(linelen <= 2) {
                        state = IN_DATA;
                        char fn_full[512];
                        snprintf(fn_full, sizeof(fn_full), "%s/%s", WFT_BASE_DIR, filename);
                        fd = open(fn_full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if(fd < 0) ESP_LOGE(TAG, "upload: open failed: %s", fn_full);
                        pos += linelen;
                    } else {
                        char line[256];
                        int cplen = linelen - 1 < (int)sizeof(line) - 1 ? linelen - 1 : (int)sizeof(line) - 1;
                        memcpy(line, buf + pos, cplen);
                        line[cplen] = 0;
                        char* fn = strstr(line, "filename=\"");
                        if(fn) {
                            fn += 10;
                            char* fe = strchr(fn, '"');
                            if(fe) {
                                int flen = fe - fn;
                                if(flen > 255) flen = 255;
                                memcpy(filename, fn, flen);
                                filename[flen] = 0;
                                ESP_LOGI(TAG, "upload: filename='%s'", filename);
                            }
                        }
                        pos += linelen;
                    }
                } else if(state == IN_DATA) {
                    int remain = n - pos;
                    int end = -1;
                    int search_end = remain - (int)blen;
                    for(int i = 0; i < search_end; i++) {
                        if(memcmp(buf + pos + i, bound_delim, blen) == 0) {
                            end = i;
                            break;
                        }
                    }
                    int write_len = (end >= 0) ? end : remain;
                    if(end >= 0) {
                        if(write_len > 2 && buf[pos + write_len - 2] == '\r' && buf[pos + write_len - 1] == '\n')
                            write_len -= 2;
                    }
                    if(fd >= 0 && write_len > 0) {
                        int wr = write(fd, buf + pos, write_len);
                        if(wr != write_len) ESP_LOGE(TAG, "upload: short write %d/%d", wr, write_len);
                    }
                    if(end >= 0) {
                        close(fd);
                        fd = -1;
                        state = SEEK_BOUNDARY;
                        pos += end;
                    } else {
                        pos = n;
                    }
                }
            }
        }
        if(fd >= 0) close(fd);
        ESP_LOGI(TAG, "upload: done, received %d/%d bytes", received, total);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char* redirect = "<meta http-equiv=\"refresh\" content=\"0;url=/\">";
    httpd_resp_send(req, redirect, strlen(redirect));
    return ESP_OK;
}

static void wft_worker(void* arg) {
    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        bt_stop_stack(bt);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    furi_record_close(RECORD_BT);

    esp_netif_init();
    esp_err_t evl = esp_event_loop_create_default();
    if(evl != ESP_OK && evl != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop: %s", esp_err_to_name(evl));
        goto cleanup;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if(!s_ap_netif) {
        ESP_LOGE(TAG, "AP netif failed");
        goto cleanup;
    }

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(s_ap_netif, &ip);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&wcfg) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init failed");
        goto cleanup;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, WFT_SSID, 32);
    ap_cfg.ap.ssid_len = strlen(WFT_SSID);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &ap_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &ap_event_handler, NULL);

    if(esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed");
        esp_wifi_deinit();
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "AP started: %s / %s", WFT_SSID, WFT_PASS);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = 80;
    hcfg.max_uri_handlers = 8;
    hcfg.max_open_sockets = 4;
    hcfg.uri_match_fn = httpd_uri_match_wildcard;

    if(httpd_start(&s_http, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        esp_wifi_stop();
        esp_wifi_deinit();
        goto cleanup;
    }

    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handler_root};
    static const httpd_uri_t down = {.uri = "/download/*", .method = HTTP_GET, .handler = handler_download};
    static const httpd_uri_t upl = {.uri = "/upload", .method = HTTP_POST, .handler = handler_upload};
    static const httpd_uri_t del = {.uri = "/delete/*", .method = HTTP_POST, .handler = handler_delete};
    httpd_register_uri_handler(s_http, &root);
    httpd_register_uri_handler(s_http, &down);
    httpd_register_uri_handler(s_http, &upl);
    httpd_register_uri_handler(s_http, &del);

    httpd_register_err_handler(s_http, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, handler_431);

    s_running = true;
    ESP_LOGI(TAG, "HTTP server running on http://192.168.4.1/");

    while(s_running) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

cleanup:
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_client_count = 0;
    s_running = false;
    s_worker = NULL;
    ESP_LOGI(TAG, "WFT stopped");

    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }

    vTaskDelete(NULL);
}

bool wft_start(void) {
    if(s_running) return true;
    if(s_worker) return true;

    xTaskCreate(wft_worker, "wft_worker", 4096, NULL, 5, &s_worker);
    if(!s_worker) return false;

    for(int i = 0; i < 100; i++) {
        if(s_running) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return s_running;
}

void wft_stop(void) {
    s_running = false;
    for(int i = 0; i < 50; i++) {
        if(!s_worker) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if(s_worker) {
        ESP_LOGW(TAG, "worker did not exit, cleaning up directly");
        if(s_http) {
            httpd_stop(s_http);
            s_http = NULL;
        }
        esp_wifi_stop();
        esp_wifi_deinit();
        s_client_count = 0;
        s_worker = NULL;
    }
    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wft_get_status(WftStatus* status) {
    if(!status) return false;
    status->running = s_running;
    status->client_count = s_client_count;
    snprintf(status->ip, sizeof(status->ip), "192.168.4.1");
    return true;
}
