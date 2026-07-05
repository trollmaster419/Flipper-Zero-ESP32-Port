#include "channel.h"
#include "config.h"
#include "../wlan_app/wlan_hal.h"
#include <esp_log.h>
#define TAG "PwnChan"
static int s_current_channel = 1;
void channel_init(int ch) {
    s_current_channel = ch;
    ESP_LOGI(TAG, "Initializing on channel %d", ch);
    wlan_hal_set_channel((uint8_t)ch);
    ESP_LOGI(TAG, "Set channel to %d", ch);
}
void channel_cycle(void) {
    int num = (int)(sizeof(g_config.channels) / sizeof(g_config.channels[0]));
    int idx = config_random(0, num - 1);
    int new_ch = g_config.channels[idx];
    channel_switch(new_ch);
}
void channel_switch(int ch) {
    ESP_LOGI(TAG, "Switching to channel %d", ch);
    wlan_hal_set_channel((uint8_t)ch);
    s_current_channel = ch;
    ESP_LOGI(TAG, "Currently on channel %d", ch);
}
void channel_check(int ch) {
    if (ch == s_current_channel) {
        ESP_LOGI(TAG, "Currently on channel %d", s_current_channel);
    } else {
        ESP_LOGW(TAG, "Expected channel %d, cached as %d", ch, s_current_channel);
    }
}
int channel_get(void) {
    return s_current_channel;
}
bool channel_is_valid(int ch) {
    for (int i = 0; i < PWNAGOTCHI_CHANNELS_COUNT; i++) {
        if (g_config.channels[i] == ch) return true;
    }
    return false;
}