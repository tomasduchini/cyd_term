#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "app_calendar.h"

#define COL_BG      0x0000
#define COL_TEXT    0xDF3F
#define COL_MAIN    0x4BFF
#define COL_DIM     0x6C16
#define COL_WEEKEND 0x9CBF  // lavanda
#define COL_SEL     0xFD80  // ambar

#define CAL_C    (FRAME_C0 + 6)   // grilla: 7 columnas de 4 celdas
#define CELL_W   4
#define TITLE_R  (FRAME_R0 + 2)
#define HDR_R    (FRAME_R0 + 3)
#define GRID_R   (FRAME_R0 + 4)   // hasta 6 semanas
#define INFO_R   (FRAME_R0 + 11)

static const char *s_dias[]  = { "domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado" };
static const char *s_meses[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                                 "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
static const char *s_hdr[]   = { "lu", "ma", "mi", "ju", "vi", "sa", "do" };

static int s_year, s_mon;   // mes mostrado (anio completo, mes 0..11)
static int s_sel;           // dia seleccionado; 0 = ninguno
static int s_day_key;       // fecha del ultimo dibujo, para detectar el cambio de dia
static int64_t s_last_check;

static bool get_today(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
    return out->tm_year + 1900 >= 2025;
}

static time_t noon(int y, int m, int d)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = m, .tm_mday = d, .tm_hour = 12, .tm_isdst = -1 };
    return mktime(&t);
}

static int days_in_month(int y, int m)  // el dia 0 del mes siguiente es el ultimo de este
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = m + 1, .tm_mday = 0, .tm_hour = 12, .tm_isdst = -1 };
    mktime(&t);
    return t.tm_mday;
}

static int first_col(int y, int m)  // columna del dia 1: 0 = lunes
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = m, .tm_mday = 1, .tm_hour = 12, .tm_isdst = -1 };
    mktime(&t);
    return (t.tm_wday + 6) % 7;
}

static void print_center(int c, int r, const char *s, int w, uint16_t fg)
{
    char line[48];
    int len = strlen(s);
    if (len > w) len = w;
    int left = (w - len) / 2;
    snprintf(line, sizeof line, "%*s%-*.*s", left, "", w - left, len, s);
    term_print(c, r, line, fg, COL_BG);
}

static void go_today(void)
{
    struct tm t;
    get_today(&t);
    s_year = t.tm_year + 1900;
    s_mon = t.tm_mon;
    s_sel = 0;
}

static void draw(void)
{
    struct tm now;
    bool synced = get_today(&now);
    char buf[48];

    // titulo y flechas
    term_print(CAL_C, TITLE_R, "[<]", COL_MAIN, COL_BG);
    snprintf(buf, sizeof buf, "%s %d", s_meses[s_mon], s_year);
    print_center(CAL_C + 4, TITLE_R, buf, 20, COL_TEXT);
    term_print(CAL_C + 25, TITLE_R, "[>]", COL_MAIN, COL_BG);

    // dias de la semana
    for (int i = 0; i < 7; i++) {
        snprintf(buf, sizeof buf, " %s ", s_hdr[i]);
        term_print(CAL_C + i * CELL_W, HDR_R, buf, i >= 5 ? COL_WEEKEND : COL_DIM, COL_BG);
    }

    // grilla
    int first = first_col(s_year, s_mon), ndays = days_in_month(s_year, s_mon);
    for (int cell = 0; cell < 42; cell++) {
        int d = cell - first + 1;
        int c = CAL_C + (cell % 7) * CELL_W, r = GRID_R + cell / 7;
        if (d < 1 || d > ndays) {
            term_print(c, r, "    ", COL_BG, COL_BG);
            continue;
        }
        bool is_today = synced && now.tm_year + 1900 == s_year && now.tm_mon == s_mon && now.tm_mday == d;
        uint16_t fg = (cell % 7 >= 5) ? COL_WEEKEND : COL_TEXT, bg = COL_BG;
        if (is_today) {
            fg = COL_BG;
            bg = COL_MAIN;
        } else if (d == s_sel) {
            fg = COL_BG;
            bg = COL_SEL;
        }
        snprintf(buf, sizeof buf, "%2d", d);
        term_put(c, r, ' ', COL_BG, COL_BG);
        term_print(c + 1, r, buf, fg, bg);
        term_put(c + 3, r, ' ', COL_BG, COL_BG);
    }

    // info del dia seleccionado (o de hoy)
    char l1[48] = "", l2[64] = "";
    if (!synced) {
        snprintf(l1, sizeof l1, "sin hora sincronizada");
    } else {
        int y = s_sel ? s_year : now.tm_year + 1900;
        int m = s_sel ? s_mon : now.tm_mon;
        int d = s_sel ? s_sel : now.tm_mday;
        time_t tt = noon(y, m, d);
        struct tm t;
        localtime_r(&tt, &t);
        snprintf(l1, sizeof l1, "%s %d de %s", s_dias[t.tm_wday], d, s_meses[m]);

        int diff = (int)lround(difftime(tt, noon(now.tm_year + 1900, now.tm_mon, now.tm_mday)) / 86400.0);
        char rel[24];
        if (diff == 0)       snprintf(rel, sizeof rel, "hoy");
        else if (diff == 1)  snprintf(rel, sizeof rel, "mañana");
        else if (diff == -1) snprintf(rel, sizeof rel, "ayer");
        else if (diff > 0)   snprintf(rel, sizeof rel, "en %d dias", diff);
        else                 snprintf(rel, sizeof rel, "hace %d dias", -diff);
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        snprintf(l2, sizeof l2, "%s - dia %d de %d", rel, t.tm_yday + 1, leap ? 366 : 365);
    }
    print_center(FRAME_C0 + 1, INFO_R, l1, 38, s_sel ? COL_SEL : COL_TEXT);
    print_center(FRAME_C0 + 1, INFO_R + 1, l2, 38, COL_DIM);

    s_day_key = synced ? (now.tm_year * 400 + now.tm_yday) : -1;
}

void cal_enter(void)
{
    go_today();
    draw();
    s_last_check = esp_timer_get_time();
}

void cal_step(void)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_check < 1000000) return;
    s_last_check = now_us;
    struct tm t;
    bool synced = get_today(&t);
    int key = synced ? (t.tm_year * 400 + t.tm_yday) : -1;
    if (key != s_day_key) draw();  // cambio el dia (o se sincronizo la hora)
}

void cal_touch(int x, int y)
{
    int col = x / 8, row = y / 16;

    if (row >= TITLE_R && row <= HDR_R) {  // titulo: flechas o volver a hoy
        if (col < CAL_C + 6) {
            if (--s_mon < 0) { s_mon = 11; s_year--; }
            s_sel = 0;
        } else if (col >= CAL_C + 22) {
            if (++s_mon > 11) { s_mon = 0; s_year++; }
            s_sel = 0;
        } else {
            go_today();
        }
    } else if (row >= GRID_R && row < GRID_R + 6 && col >= CAL_C && col < CAL_C + 7 * CELL_W) {
        int cell = (row - GRID_R) * 7 + (col - CAL_C) / CELL_W;
        int d = cell - first_col(s_year, s_mon) + 1;
        if (d < 1 || d > days_in_month(s_year, s_mon)) return;
        s_sel = (s_sel == d) ? 0 : d;
    } else {
        return;
    }
    draw();
}
















