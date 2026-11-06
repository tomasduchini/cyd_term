#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "pages.h"
#include "app_icons.h"
#include "app_stopwatch.h"
#include "page_apps.h"
#include "app_timer.h"
#include "app_calendar.h"

#define COL_BG    0x0000
#define COL_TEXT  0xDF3F
#define COL_MAIN  0x4BFF
#define COL_DIM   0x6C16
#define COL_OFF   0x3186  // iconos de apps que todavia no existen
#define ACCENT    0x4BFF

#define TILE_C0  (FRAME_C0 + 2)
#define TILE_R0  (FRAME_R0 + 2)
#define TILE_W   12
#define TILE_H   6
#define TOAST_US (1500 * 1000)

typedef struct {
    const char *title;
    void (*enter)(void);
    void (*step)(void);
    void (*touch)(int x, int y);
} app_t;

// Mismo orden que APP_ICONS en tools/gen_font.py
static const app_t s_apps[] = {
    { "cronometro",   sw_enter, sw_step, sw_touch },
    { "temporizador", tm_enter, tm_step, tm_touch },
    { "calendario",   cal_enter, cal_step, cal_touch },
    { "musica",       NULL,     NULL,    NULL     },
    { "juegos",       NULL,     NULL,    NULL     },
};
#define N_APPS ((int)(sizeof s_apps / sizeof s_apps[0]))
_Static_assert(N_APPS == APP_ICON_COUNT, "s_apps y APP_ICONS tienen que coincidir");

static int s_open = -1;  // app abierta; -1 = lanzador
static int s_toast = -1;
static int64_t s_toast_until;

static void print_center(int c, int r, const char *s, int w, uint16_t fg)
{
    char line[40];
    int len = strlen(s);
    if (len > w) len = w;
    int left = (w - len) / 2;
    snprintf(line, sizeof line, "%*s%-*.*s", left, "", w - left, len, s);
    term_print(c, r, line, fg, COL_BG);
}

static void draw_tile(int i)
{
    int x0 = TILE_C0 + (i % 3) * TILE_W;
    int y0 = TILE_R0 + (i / 3) * TILE_H;
    bool ready = s_apps[i].enter != NULL;
    for (int t = 0; t < 8; t++) {
        term_put(x0 + 4 + t % 4, y0 + t / 4, app_icon_tiles[i][t], ready ? COL_MAIN : COL_OFF, COL_BG);
    }
    print_center(x0, y0 + 3, s_apps[i].title, TILE_W, ready ? COL_TEXT : COL_DIM);
    print_center(x0, y0 + 4, s_toast == i ? "pronto" : "", TILE_W, COL_DIM);
}

static void draw_launcher(void)
{
    for (int i = 0; i < N_APPS; i++) draw_tile(i);
}

static void open_app(int i)
{
    s_open = i;
    frame_draw(s_apps[i].title, ACCENT, true);
    term_print(FRAME_C0 + 2, FRAME_R0 + 1, "< volver", COL_DIM, COL_BG);
    s_apps[i].enter();
}

void apps_enter(void)
{
    s_open = -1;
    s_toast = -1;
    draw_launcher();
}

void apps_step(void)
{
    if (s_open >= 0) {
        if (s_apps[s_open].step) s_apps[s_open].step();
        return;
    }
    if (s_toast >= 0 && esp_timer_get_time() > s_toast_until) {
        int i = s_toast;
        s_toast = -1;
        draw_tile(i);
    }
}

void apps_leave(void)
{
    s_open = -1;  // el estado de cada app sigue en su modulo
}

void apps_touch(int x, int y)
{
    int col = x / 8, row = y / 16;

    if (s_open >= 0) {
        if (row <= FRAME_R0 + 1 && col < FRAME_C0 + 14) {  // "< volver"
            s_open = -1;
            pages_redraw();
            return;
        }
        if (s_apps[s_open].touch) s_apps[s_open].touch(x, y);
        return;
    }

    int tx = (col - TILE_C0) / TILE_W, ty = (row - TILE_R0) / TILE_H;
    if (col < TILE_C0 || row < TILE_R0 || tx > 2 || ty > 1) return;
    int i = ty * 3 + tx;
    if (i >= N_APPS) return;
    if (s_apps[i].enter) {
        open_app(i);
    } else {
        int prev = s_toast;
        s_toast = i;
        s_toast_until = esp_timer_get_time() + TOAST_US;
        if (prev >= 0 && prev != i) draw_tile(prev);
        draw_tile(i);
    }
}
