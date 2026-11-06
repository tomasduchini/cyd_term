#pragma once
#include <stdbool.h>

void tm_enter(void);
void tm_step(void);
void tm_touch(int x, int y);
bool timer_take_expired(void);  // true una sola vez cuando vence (llamar en todos los frames)
