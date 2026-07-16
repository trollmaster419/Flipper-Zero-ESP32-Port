#pragma once
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#define TAG "Pwnagotchi"
typedef enum {
    EventTypeTick,
    EventTypeKey,
    EventTypePwnDetected,
    EventTypePwnLost,
    EventTypeDeauthDone,
    EventTypeAdvertDone,
    EventTypeEpochDone,
    EventTypeWifiError,
} EventType;
typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;
typedef enum {
    MoodSleeping,
    MoodAwakening,
    MoodAwake,
    MoodObserving,
    MoodObservingHappy,
    MoodIntense,
    MoodCool,
    MoodHappy,
    MoodGrateful,
    MoodExcited,
    MoodSmart,
    MoodFriendly,
    MoodMotivated,
    MoodDemotivated,
    MoodBored,
    MoodSad,
    MoodLonely,
    MoodBroken,
    MoodDebug,
} PwnagotchiMood;
typedef struct {
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    PwnagotchiMood mood;
    uint32_t handshakes;
    uint32_t uptime_secs;
    uint32_t pwnd_run;
    uint32_t pwnd_tot;
    int current_channel;
    bool wifi_started;
    bool pwnagotchi_detected;
    char peer_name[64];
    char peer_type[16];
    char peer_pwnd[16];
    char ap_name[33];
    uint8_t ap_bssid[6];
    char epoch_status[32];
    bool worker_running;
    int epochs_since_peer;
    int epochs_total;
    int handshakes_this_session;
    int missed_deauths;
    int aps_now;
    int aps_total;
} PwnagotchiState;