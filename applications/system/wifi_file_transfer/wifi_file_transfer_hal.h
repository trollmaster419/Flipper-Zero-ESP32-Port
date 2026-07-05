#pragma once
#include <stdbool.h>

typedef struct {
    char ip[16];
    int client_count;
    bool running;
} WftStatus;

bool wft_start(void);
void wft_stop(void);
bool wft_get_status(WftStatus* status);
