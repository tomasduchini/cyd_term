#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "bigclock.h"
#include "app_timer.h"

#define COL_BG    0x0000
#define COL_TEXT  0xDF3F
#define COL_MAIN  0x4BFF
#define COL_DIM   0x6C16
#define COL_OFF   0x3186
#define COL_WARN  0xFD80

#define TM_C    (FRAME_C0 + 11)   // 17 celdas, centrado
#define TM_R    (FRAME_R0 + 3)
#define ADJ_R   (FRAME_R0 + 7)
#define ACT_R   (FRAME_R0 + 9)
#define INFO_R  (FRAME_R0 + 11)
#define MAX_S   (99 * 60 + 59)

typedef enum { T_IDLE, T_RUN, T_PAUSE, T_DONE } tstate_t;

static tstate_t s_st = T_IDLE, s_drawn_st = (tstate_t)-1;
static int s_set_s = 300;       // duracion elegida
static int64_t s_end_us;        // fin, mientras corre
static int64_t s_left_us;       // restante, en pausa

static int64_t remaining_us(void)
{
    switch (s_st) {
    case T_RUN: {
        int64_t r = s_end_us - esp_timer_get_time();
        return r > 0 ? r : 0;
    }
    case T_PAUSE: return s_left_us;
    case T_DONE:  return 0;
    default:      return (int64_t)s_set_s * 1000000;
    }
}

bool timer_take_expired(void)
{
    if (s_st == T_RUN && esp_timer_get_time() >= s_end_us) {
        s_st = T_DONE;
        return true;
    }
    return false;
}

static void draw_time(void)
{
    int secs = (int)((remaining_us() + 999999) / 1000000);  // redondeo hacia arriba
    uint16_t fg = COL_TEXT;
    if (s_st == T_DONE) {
        fg = COL_WARN;
    } else if (s_st == T_PAUSE && (esp_timer_get_time() / 500000) % 2) {
        fg = COL_DIM;  // parpadea en pausa
    }
    bigclock_draw_pair(TM_C, TM_R, secs / 60, secs % 60, true, fg);
}

static void draw_controls(void)
{
    bool adj = (s_st == T_IDLE || s_st == T_DONE);
    uint16_t a = adj ? COL_MAIN : COL_OFF;
    term_print(FRAME_C0 + 4,  ADJ_R, "[-1m]",  a, COL_BG);
    term_print(FRAME_C0 + 11, ADJ_R, "[+1m]",  a, COL_BG);
    term_print(FRAME_C0 + 19, ADJ_R, "[-10s]", a, COL_BG);
    term_print(FRAME_C0 + 27, ADJ_R, "[+10s]", a, COL_BG);

    const char *l, *r;
    switch (s_st) {
    case T_RUN:   l = "[  pausar   ]"; r = "[ cancelar  ]"; break;
    case T_PAUSE: l = "[  seguir   ]"; r = "[ cancelar  ]"; break;
    case T_DONE:  l = "[  repetir  ]"; r = "             "; break;
    default:      l = "[  iniciar  ]"; r = "             "; break;
    }
    term_print(FRAME_C0 + 6,  ACT_R, l, COL_MAIN, COL_BG);
    term_print(FRAME_C0 + 21, ACT_R, r, COL_MAIN, COL_BG);

    char line[40] = "", pad[40];
    if (s_st == T_RUN) {
        time_t end = time(NULL) + (time_t)((remaining_us() + 999999) / 1000000);
        struct tm tm;
        localtime_r(&end, &tm);
        snprintf(line, sizeof line, "termina a las %02d:%02d", tm.tm_hour, tm.tm_min);
    } else if (s_st == T_DONE) {
        snprintf(line, sizeof line, "tiempo cumplido");
    }
    snprintf(pad, sizeof pad, "%-26.26s", line);
    term_print(FRAME_C0 + 6, INFO_R, pad, COL_DIM, COL_BG);
    s_drawn_st = s_st;
}

void tm_enter(void)
{
    draw_time();
    draw_controls();
}

void tm_step(void)
{
    draw_time();
    if (s_st != s_drawn_st) draw_controls();  // p. ej. vencio con la app abierta
}

void tm_touch(int x, int y)
{
    int col = x / 8, row = y / 16;
    int64_t now = esp_timer_get_time();

    if (row >= ADJ_R - 1 && row <= ADJ_R) {  // ajustes de duracion
        if (s_st != T_IDLE && s_st != T_DONE) return;
        int d = (col < FRAME_C0 + 10) ? -60 : (col < FRAME_C0 + 18) ? 60 : (col < FRAME_C0 + 26) ? -10 : 10;
        s_set_s += d;
        if (s_set_s < 10) s_set_s = 10;
        if (s_set_s > MAX_S) s_set_s = MAX_S;
        s_st = T_IDLE;
    } else if (row >= ACT_R - 1 && row <= ACT_R + 1) {  // acciones
        bool left = col < FRAME_C0 + 20;
        switch (s_st) {
        case T_IDLE:
        case T_DONE:
            if (!left) return;
            s_end_us = now + (int64_t)s_set_s * 1000000;
            s_st = T_RUN;
            break;
        case T_RUN:
            if (left) {
                s_left_us = s_end_us - now;
                s_st = T_PAUSE;
            } else {
                s_st = T_IDLE;
            }
            break;
        case T_PAUSE:
            if (left) {
                s_end_us = now + s_left_us;
                s_st = T_RUN;
            } else {
                s_st = T_IDLE;
            }
            break;
        }
    } else {
        return;
    }
    draw_time();
    draw_controls();
}
