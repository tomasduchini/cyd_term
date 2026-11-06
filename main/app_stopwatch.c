#include <stdio.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "bigclock.h"
#include "app_stopwatch.h"

#define COL_BG    0x0000
#define COL_TEXT  0xDF3F  // azul muy claro
#define COL_MAIN  0x4BFF  // azul electrico
#define COL_DIM   0x6C16

#define SW_C    (FRAME_C0 + 9)    // reloj grande (17 celdas) + parte chica (3)
#define SW_R    (FRAME_R0 + 3)
#define BTN_R   (FRAME_R0 + 8)
#define BTN_L   (FRAME_C0 + 6)
#define BTN_RT  (FRAME_C0 + 21)
#define BTN_SPLIT (FRAME_C0 + 20)
#define LAP_R   (FRAME_R0 + 10)
#define N_LAPS  3

// Estado: vive aca, no en la pantalla, asi sigue corriendo en otras paginas
static bool s_running;
static int64_t s_start_us;           // cuando arranco el tramo actual
static int64_t s_acc_us;             // acumulado de tramos anteriores
static int64_t s_laps[N_LAPS];       // tiempos totales de las ultimas vueltas (la mas nueva primero)
static int s_nlaps;

static int64_t elapsed(void)
{
    return s_acc_us + (s_running ? esp_timer_get_time() - s_start_us : 0);
}

static void draw_time(void)
{
    int64_t t = elapsed();
    int cs = (int)(t / 10000 % 100);
    int s  = (int)(t / 1000000 % 60);
    int m  = (int)(t / 60000000 % 60);
    int h  = (int)(t / 3600000000LL % 100);
    char buf[8];
    if (h == 0) {
        bigclock_draw_pair(SW_C, SW_R, m, s, true, COL_TEXT);
        snprintf(buf, sizeof buf, ".%02d", cs);
    } else {
        bigclock_draw_pair(SW_C, SW_R, h, m, true, COL_TEXT);
        snprintf(buf, sizeof buf, ":%02d", s);
    }
    term_print(SW_C + 18, SW_R + 2, buf, COL_MAIN, COL_BG);
}

static void draw_buttons(void)
{
    const char *l, *r;
    if (s_running)          { l = "[  pausar   ]"; r = "[  vuelta   ]"; }
    else if (elapsed() > 0) { l = "[  seguir   ]"; r = "[ reiniciar ]"; }
    else                    { l = "[  iniciar  ]"; r = "             "; }
    term_print(BTN_L,  BTN_R, l, COL_MAIN, COL_BG);
    term_print(BTN_RT, BTN_R, r, COL_MAIN, COL_BG);
}

static void fmt_t(char *buf, size_t n, int64_t t)  // mm:ss.cc
{
    snprintf(buf, n, "%02d:%02d.%02d", (int)(t / 60000000 % 100), (int)(t / 1000000 % 60), (int)(t / 10000 % 100));
}

static void draw_laps(void)
{
    char line[40], a[12], b[12];
    for (int i = 0; i < N_LAPS; i++) {
        if (i < s_nlaps) {
            int64_t split = s_laps[i] - ((i + 1 < s_nlaps) ? s_laps[i + 1] : 0);
            fmt_t(a, sizeof a, s_laps[i]);
            fmt_t(b, sizeof b, split);
            snprintf(line, sizeof line, "v%-2d %s  +%s", s_nlaps - i, a, b);
        } else {
            line[0] = '\0';
        }
        char pad[40];
        snprintf(pad, sizeof pad, "%-24.24s", line);
        term_print(SW_C, LAP_R + i, pad, i == 0 ? COL_TEXT : COL_DIM, COL_BG);
    }
}

void sw_enter(void)
{
    draw_time();
    draw_buttons();
    draw_laps();
}

void sw_step(void)
{
    draw_time();  // solo se redibujan las celdas que cambian
}

void sw_touch(int x, int y)
{
    int row = y / 16, col = x / 8;
    if (row < BTN_R - 1 || row > BTN_R + 1) return;
    int64_t now = esp_timer_get_time();

    if (col < BTN_SPLIT) {  // iniciar / pausar / seguir
        if (s_running) {
            s_acc_us += now - s_start_us;
            s_running = false;
        } else {
            s_start_us = now;
            s_running = true;
        }
    } else if (s_running) {  // vuelta
        for (int i = N_LAPS - 1; i > 0; i--) s_laps[i] = s_laps[i - 1];
        s_laps[0] = elapsed();
        if (s_nlaps < 99) s_nlaps++;
    } else if (elapsed() > 0) {  // reiniciar
        s_acc_us = 0;
        s_nlaps = 0;
    }
    draw_time();
    draw_buttons();
    draw_laps();
}
