#pragma once
#include <stdbool.h>
#include <stdint.h>
void channel_init(int ch);
void channel_cycle(void);
void channel_switch(int ch);
int channel_get(void);
void channel_check(int ch);
bool channel_is_valid(int ch);