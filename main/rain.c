#include "esp_random.h"
#include "term.h"
#include "rain.h"

#define COL_HEAD 0xBFF9   // verde muy claro
#define COL_BODY 0x1508   // verde medio
#define COL_TAIL 0x0AE5   // verde oscuro
#define COL_BG   0x0000
#define GLITCHES_PER_FRAME 2

typedef struct {
    int head;    // fila de la cabeza; negativa = demora antes de entrar
    int len;     // largo de la estela
    int period;  // frames entre avances
    int tick;
} drop_t;

static drop_t drops[TERM_COLS];

// Rectangulo enmascarado (inclusivo); arranca vacio
static int m_c0 = 1, m_r0 = 1, m_c1 = 0, m_r1 = 0;

void rain_set_mask(int c0, int r0, int c1, int r1)
{
    m_c0 = c0; m_r0 = r0; m_c1 = c1; m_r1 = r1;
}

static bool masked(int c, int r)
{
    return c >= m_c0 && c <= m_c1 && r >= m_r0 && r <= m_r1;
}

static void r_put(int c, int r, uint8_t ch, uint16_t fg, uint16_t bg)
{
    if (!masked(c, r)) term_put(c, r, ch, fg, bg);
}

static void r_set_fg(int c, int r, uint16_t fg)
{
    if (!masked(c, r)) term_set_fg(c, r, fg);
}

static void r_set_ch(int c, int r, uint8_t ch)
{
    if (!masked(c, r)) term_set_ch(c, r, ch);
}

static int rnd(int lo, int hi)  // [lo, hi]
{
    return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

static uint8_t rnd_char(void)
{
    return (uint8_t)rnd(0x21, 0x7E);
}

static void drop_reset(drop_t *d)
{
    d->head   = -rnd(1, 20);
    d->len    = rnd(6, 13);
    d->period = rnd(1, 4);
    d->tick   = 0;
}

void rain_init(void)
{
    term_clear(COL_BG);
    for (int c = 0; c < TERM_COLS; c++) drop_reset(&drops[c]);
}

void rain_step(void)
{
    for (int c = 0; c < TERM_COLS; c++) {
        drop_t *d = &drops[c];
        if (++d->tick < d->period) continue;
        d->tick = 0;
        d->head++;

        r_set_fg(c, d->head - 1, COL_BODY);
        r_set_fg(c, d->head - d->len / 2, COL_TAIL);
        r_put(c, d->head - d->len, ' ', COL_BODY, COL_BG);
        r_put(c, d->head, rnd_char(), COL_HEAD, COL_BG);

        if (d->head - d->len >= TERM_ROWS) drop_reset(d);
    }

    for (int i = 0; i < GLITCHES_PER_FRAME; i++) {
        int c = rnd(0, TERM_COLS - 1);
        const drop_t *d = &drops[c];
        r_set_ch(c, d->head - rnd(1, d->len - 1), rnd_char());
    }
}
