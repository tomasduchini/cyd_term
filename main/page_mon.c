#include <stdio.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "term.h"
#include "frame.h"
#include "console.h"
#include "netmon.h"
#include "page_mon.h"

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

static uint32_t s_seq;
static int s_frame_us;
static int64_t s_last_sys;

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
    draw_ping(ROW_GW,  d->gw);
    draw_ping(ROW_NET, d->inet);
    draw_rssi(ROW_RSSI, d->rssi);
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
    netmon_t d;
    draw_static();
    if (netmon_get(&d)) {
        s_seq = d.seq;
    } else {  // todavia sin red: bases tenues
        for (int i = 0; i < NETMON_N; i++) {
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
        draw_series(&d);
    }

    int64_t now = esp_timer_get_time();
    if (now - s_last_sys >= 1000000) {
        s_last_sys = now;
        draw_sys();
    }
}

void mon_leave(void)
{
    con_hide();
}
