#include "term.h"
#include "bigclock.h"

#define COL_BG   0x0000
#define CH_FULL  0x80  // █
#define CH_UP    0x8C  // ▀
#define CH_LOW   0x8D  // ▄
#define DIG_DASH 10

// Digitos de 3x5 "pixeles"; cada fila es un valor de 3 bits, bit2 = columna izquierda
static const uint8_t s_digits[11][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
    {0,0,7,0,0},  // guion
};

static bool px(int d, int y, int x)  // y: 0..4, x: 0..2
{
    if (d < 0 || y > 4) return false;  // d < 0: digito en blanco
    return (s_digits[d][y] >> (2 - x)) & 1;
}

static uint8_t half_block(bool top, bool bot)
{
    if (top && bot) return CH_FULL;
    if (top)        return CH_UP;
    if (bot)        return CH_LOW;
    return ' ';
}

static void big_digit(int c, int r, int d, uint16_t fg)
{
    for (int cy = 0; cy < 3; cy++) {
        for (int x = 0; x < 3; x++) {
            uint8_t ch = half_block(px(d, 2 * cy, x), px(d, 2 * cy + 1, x));
            term_put(c + x, r + cy, ch, ch == ' ' ? COL_BG : fg, COL_BG);
        }
    }
}

static void big_colon(int c, int r, bool on, uint16_t fg)
{
    term_put(c, r,     on ? CH_LOW : ' ', on ? fg : COL_BG, COL_BG);
    term_put(c, r + 1, on ? CH_LOW : ' ', on ? fg : COL_BG, COL_BG);
}

void bigclock_draw(int c, int r, const struct tm *tm, bool synced, bool colon_on, uint16_t fg)
{
    int d[4] = { DIG_DASH, DIG_DASH, DIG_DASH, DIG_DASH };
    if (synced) {
        d[0] = tm->tm_hour / 10;
        d[1] = tm->tm_hour % 10;
        d[2] = tm->tm_min / 10;
        d[3] = tm->tm_min % 10;
    }
    big_digit(c,      r, d[0], fg);
    big_digit(c + 4,  r, d[1], fg);
    big_colon(c + 8,  r, colon_on, fg);
    big_digit(c + 10, r, d[2], fg);
    big_digit(c + 14, r, d[3], fg);
}

void bignum_draw(int c, int r, int value, int ndigits, uint16_t fg)
{
    int max = 1;
    for (int i = 0; i < ndigits; i++) max *= 10;
    if (value >= max) value = max - 1;

    for (int j = 0; j < ndigits; j++) {  // j = 0 es el digito de la izquierda
        int k = ndigits - 1 - j;          // posicion desde la derecha
        int d;
        if (value < 0) {
            d = DIG_DASH;
        } else {
            int p = 1;
            for (int i = 0; i < k; i++) p *= 10;
            d = (k > 0 && value < p) ? -1 : (value / p) % 10;  // ceros a la izquierda en blanco
        }
        big_digit(c + 4 * j, r, d, fg);
    }
}
