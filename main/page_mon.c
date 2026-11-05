#include <stdio.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "term.h"
#include "frame.h"
#include "console.h"
#include "netmon.h"
#include "page_mon.h"
#include <limits.h>
#include "pages.h"

#define COL_BG    0x0000
#define COL_TEXT  0xCFFB  // verde muy claro
#define COL_MAIN  0x3EF0  // verde
#define COL_DIM   0x5CCD  // verde apagado
#define COL_TRACK 0x19C4  // verde oscuro
#define COL_WARN  0xFD80  // ambar
#define COL_BAD   0xFA08  // rojo

#define CH_H     0x88  // ─
#define CH_VBAR1 0xE0  // barra vertical angosta, niveles 1..8

#define ROW_GW   (FRAME_R0 + 2)
#define ROW_NET  (FRAME_R0 + 3)
#define ROW_RSSI (FRAME_R0 + 4)
#define ROW_SYS  (FRAME_R0 + 6)
#define ROW_SEP  (FRAME_R0 + 7)   // la consola ocupa de la fila siguiente hasta abajo
#define X_LBL    (FRAME_C0 + 2)
#define X_SPK    (FRAME_C0 + 9)   // NETMON_N celdas
#define X_VAL    (FRAME_C0 + 34)  // 5 caracteres

// Vista ampliada
#define EXP_C0  (FRAME_C0 + 3)
#define EXP_W   34                       // columnas del grafico
#define EXP_R0  (FRAME_R0 + 3)           // fila superior del grafico
#define EXP_H   8                        // filas de alto: 64 niveles
#define EXP_AGG (NETMON_HIST / EXP_W)    // muestras por columna

static uint32_t s_seq;
static int s_frame_us;
static int64_t s_last_sys;
static int s_exp = -1;  // serie ampliada: 0 router, 1 inet, 2 senal; -1 = vista normal
static const char *s_names[3] = { "router", "inet", "senal" };

void mon_set_frame_us(int us)
{
    s_frame_us = us;
}

static uint16_t ping_color(int ms)
{
    if (ms < 0)   return COL_BAD;
    if (ms < 60)  return COL_MAIN;
    if (ms < 150) return COL_WARN;
    return COL_BAD;
}

static void draw_ping(int r, const int16_t *v)
{
    int top = 20;  // escala minima: 20 ms
    for (int i = 0; i < NETMON_N; i++) {
        if (v[i] > top) top = v[i];
    }
    for (int i = 0; i < NETMON_N; i++) {
        uint8_t ch;
        uint16_t fg;
        if (v[i] == NETMON_NODATA) {
            ch = CH_VBAR1;
            fg = COL_TRACK;
        } else if (v[i] < 0) {  // sin respuesta
            ch = CH_VBAR1 + 7;
            fg = COL_BAD;
        } else {
            ch = CH_VBAR1 + v[i] * 7 / top;  // niveles 1..8
            fg = ping_color(v[i]);
        }
        term_put(X_SPK + i, r, ch, fg, COL_BG);
    }

    int last = v[NETMON_N - 1];
    char buf[12];
    if (last == NETMON_NODATA) {
        snprintf(buf, sizeof buf, "  -- ");
    } else if (last < 0) {
        snprintf(buf, sizeof buf, "perd ");
    } else {
        snprintf(buf, sizeof buf, "%3dms", last > 999 ? 999 : last);
    }
    term_print(X_VAL, r, buf, last == NETMON_NODATA ? COL_DIM : ping_color(last), COL_BG);
}

static void draw_rssi(int r, const int16_t *v)
{
    for (int i = 0; i < NETMON_N; i++) {
        if (v[i] == NETMON_NODATA) {
            term_put(X_SPK + i, r, CH_VBAR1, COL_TRACK, COL_BG);
            continue;
        }
        int lv = 1 + (v[i] + 90) * 7 / 60;  // escala fija -90..-30 dBm
        if (lv < 1) lv = 1;
        if (lv > 8) lv = 8;
        term_put(X_SPK + i, r, CH_VBAR1 + lv - 1, COL_MAIN, COL_BG);
    }

    int last = v[NETMON_N - 1];
    char buf[12];
    if (last == NETMON_NODATA) {
        snprintf(buf, sizeof buf, " -- ");
    } else {
        snprintf(buf, sizeof buf, "%4d", last < -99 ? -99 : last);
    }
    term_print(X_VAL, r, buf, last == NETMON_NODATA ? COL_DIM : COL_TEXT, COL_BG);
}

static void draw_series(const netmon_t *d)
{
    const int o = NETMON_HIST - NETMON_N;  // las sparklines muestran las ultimas NETMON_N
    draw_ping(ROW_GW,  d->gw + o);
    draw_ping(ROW_NET, d->inet + o);
    draw_rssi(ROW_RSSI, d->rssi + o);
}


static void clear_inner(void)
{
    for (int r = FRAME_R0 + 1; r < FRAME_R1; r++) {
        for (int c = FRAME_C0 + 1; c < FRAME_C1; c++) term_put(c, r, ' ', COL_BG, COL_BG);
    }
}

static void draw_expanded(const netmon_t *d)
{
    const int16_t *v = (s_exp == 0) ? d->gw : (s_exp == 1) ? d->inet : d->rssi;
    bool ping = s_exp < 2;
    char buf[48], line[48];

    // estadisticas de toda la ventana
    int vmin = INT_MAX, vmax = INT_MIN, nval = 0, nloss = 0;
    long sum = 0;
    for (int i = 0; i < NETMON_HIST; i++) {
        int s = v[i];
        if (s == NETMON_NODATA) continue;
        if (ping && s < 0) {
            nloss++;
            continue;
        }
        if (s < vmin) vmin = s;
        if (s > vmax) vmax = s;
        sum += s;
        nval++;
    }

    // escala
    int bot, top;
    if (ping) {
        static const int nice[] = { 20, 50, 100, 200, 500, 1000 };
        bot = 0;
        top = (vmax > 1000) ? vmax : 1000;
        for (int i = 0; i < 6; i++) {
            if (vmax <= nice[i]) {
                top = nice[i];
                break;
            }
        }
    } else {
        bot = -90;
        top = -30;
    }

    // titulo y escala
    snprintf(buf, sizeof buf, "%s  ultimos %d min", s_names[s_exp], NETMON_HIST * 2 / 60);
    snprintf(line, sizeof line, "%-22.22s", buf);
    term_print(EXP_C0, FRAME_R0 + 1, line, COL_TEXT, COL_BG);
    term_print(EXP_C0 + EXP_W - 12, FRAME_R0 + 1, "toca: volver", COL_DIM, COL_BG);
    if (ping) snprintf(buf, sizeof buf, "%d ms", top);
    else      snprintf(buf, sizeof buf, "%d dBm", top);
    snprintf(line, sizeof line, "%-10.10s", buf);
    term_print(EXP_C0, FRAME_R0 + 2, line, COL_DIM, COL_BG);

    // columnas
    for (int col = 0; col < EXP_W; col++) {
        int agg = INT_MIN, cnt = 0;
        long acc = 0;
        bool loss = false;
        for (int k = 0; k < EXP_AGG; k++) {
            int s = v[col * EXP_AGG + k];
            if (s == NETMON_NODATA) continue;
            if (ping && s < 0) {
                loss = true;
                continue;
            }
            if (s > agg) agg = s;
            acc += s;
            cnt++;
        }
        if (!ping && cnt) agg = (int)(acc / cnt);  // senal: promedio; pings: peor valor

        int level;
        uint16_t fg;
        if (loss) {
            level = EXP_H * 8;
            fg = COL_BAD;
        } else if (!cnt) {
            level = -1;  // sin datos: solo la base
            fg = COL_TRACK;
        } else {
            level = 1 + (agg - bot) * (EXP_H * 8 - 1) / (top - bot);
            if (level < 1) level = 1;
            if (level > EXP_H * 8) level = EXP_H * 8;
            fg = ping ? ping_color(agg) : COL_MAIN;
        }

        for (int rr = 0; rr < EXP_H; rr++) {  // de abajo hacia arriba
            int r = EXP_R0 + EXP_H - 1 - rr;
            int part = level - rr * 8;
            uint8_t ch;
            if (level < 0) {
                ch = (rr == 0) ? CH_VBAR1 : ' ';
            } else if (part <= 0) {
                ch = ' ';
            } else {
                ch = CH_VBAR1 + (part >= 8 ? 8 : part) - 1;
            }
            term_put(EXP_C0 + col, r, ch, ch == ' ' ? COL_BG : fg, COL_BG);
        }
    }

    // eje de tiempo
    term_print(EXP_C0, EXP_R0 + EXP_H, "-4 min", COL_DIM, COL_BG);
    term_print(EXP_C0 + EXP_W - 5, EXP_R0 + EXP_H, "ahora", COL_DIM, COL_BG);

    // estadisticas
    if (nval) {
        snprintf(buf, sizeof buf, "min %d  prom %d  max %d %s",
                 vmin, (int)(sum / nval), vmax, ping ? "ms" : "dBm");
    } else {
        snprintf(buf, sizeof buf, "sin datos");
    }
    snprintf(line, sizeof line, "%-34.34s", buf);
    term_print(EXP_C0, EXP_R0 + EXP_H + 1, line, COL_TEXT, COL_BG);

    if (ping) {
        int total = nval + nloss;
        if (total) snprintf(buf, sizeof buf, "perdidos %d%%  (%d de %d)", nloss * 100 / total, nloss, total);
        else       snprintf(buf, sizeof buf, " ");
        snprintf(line, sizeof line, "%-34.34s", buf);
        term_print(EXP_C0, EXP_R0 + EXP_H + 2, line, nloss ? COL_WARN : COL_DIM, COL_BG);
    }
}

static void draw_sys(void)
{
    char buf[96], line[40];
    int64_t up = esp_timer_get_time() / 1000000;
    int h = (int)(up / 3600);
    if (h > 999) h = 999;
    int m = (int)(up / 60 % 60);
    int s = (int)(up % 60);
    unsigned heap = esp_get_free_heap_size() / 1024;

    snprintf(buf, sizeof buf, "up %02d:%02d:%02d  heap %uK  frame %d.%dms",
             h, m, s, heap, s_frame_us / 1000, (s_frame_us / 100) % 10);
    snprintf(line, sizeof line, "%-36.36s", buf);
    term_print(X_LBL, ROW_SYS, line, COL_TEXT, COL_BG);
}

static void draw_static(void)
{
    term_print(X_LBL, ROW_GW,   "router", COL_DIM, COL_BG);
    term_print(X_LBL, ROW_NET,  "inet",   COL_DIM, COL_BG);
    term_print(X_LBL, ROW_RSSI, "senal",  COL_DIM, COL_BG);

    for (int c = FRAME_C0 + 1; c < FRAME_C1; c++) {
        bool on = (c - FRAME_C0) % 2;
        term_put(c, ROW_SEP, on ? CH_H : ' ', on ? COL_TRACK : COL_BG, COL_BG);
    }
    term_print(FRAME_C0 + 3, ROW_SEP, " log ", COL_DIM, COL_BG);
}

void mon_enter(void)
{
	s_exp = -1;
    netmon_t d;
    draw_static();
    if (netmon_get(&d)) {
        s_seq = d.seq;
    } else {  // todavia sin red: bases tenues
        for (int i = 0; i < NETMON_HIST; i++) {
            d.gw[i] = d.inet[i] = d.rssi[i] = NETMON_NODATA;
        }
        s_seq = 0;
    }
    draw_series(&d);
    draw_sys();
    s_last_sys = esp_timer_get_time();
    con_show();
}

void mon_step(void)
{
    netmon_t d;
    if (netmon_get(&d) && d.seq != s_seq) {
        s_seq = d.seq;
        if (s_exp >= 0) draw_expanded(&d);
        else            draw_series(&d);
    }

    int64_t now = esp_timer_get_time();
    if (s_exp < 0 && now - s_last_sys >= 1000000) {  // en la vista ampliada no hay fila de sistema
        s_last_sys = now;
        draw_sys();
    }
}

void mon_leave(void)
{
    s_exp = -1;
    con_hide();
}


void mon_touch(int x, int y)
{
    if (s_exp >= 0) {  // cualquier toque vuelve a la vista normal
        s_exp = -1;
        pages_redraw();
        return;
    }
    int k = y / 16 - ROW_GW;  // 0 router, 1 inet, 2 senal
    if (k < 0 || k > 2) return;
    netmon_t d;
    if (!netmon_get(&d)) return;
    s_exp = k;
    con_hide();
    clear_inner();
    draw_expanded(&d);
}
