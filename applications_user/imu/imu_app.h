#pragma once

#include <furi.h>
#include <input/input.h>

#define TAG "ImuApp"

typedef enum { EventTypeTick, EventTypeKey } EventType;
typedef struct { EventType type; InputEvent input; } PluginEvent;
typedef struct { FuriMutex* mutex; FuriMessageQueue* event_queue; } ImuAppState;
