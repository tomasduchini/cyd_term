#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define BIGCLOCK_W 17   // HH:MM en celdas
#define BIGCLOCK_H 3

void bigclock_draw(int c, int r, const struct tm *tm, bool synced, bool colon_on, uint16_t fg);
void bignum_draw(int c, int r, int value, int ndigits, uint16_t fg);  // value < 0: guiones
