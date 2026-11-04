#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "term.h"
#include "frame.h"
#include "settings.h"
#include "provision.h"
#include "page_cfg.h"
#include "touch_cal.h"
#include "pages.h"

#define COL_BG    0x0000
#define COL_TEXT  0xF77B  // marfil
#define COL_MAIN  0xD549  // laton
#define COL_DIM   0x8BA9  // laton apagado
#define COL_TRACK 0x3984
#define COL_WARN  0xFA08  // rojo

#define CH_H 0x88  // ─

#define X_LBL     (FRAME_C0 + 2)
#define X_MINUS   (FRAME_C0 + 20)  // "[-]"
#define X_VAL     (FRAME_C0 + 24)  // 7 caracteres
#define X_PLUS    (FRAME_C0 + 32)  // "[+]"
#define X_SPLIT   (FRAME_C0 + 28)  // tocar a la izquierda resta, a la derecha suma
#define X_CTRL    (FRAME_C0 + 18)  // mas a la izquierda es la etiqueta: no hace nada

#define ROW0      (FRAME_R0 + 2)
#define ROW_STEP  2
#define N_ITEMS   4
#define ROW_BTN   (ROW0 + N_ITEMS * ROW_STEP)
#define ROW_SEP   (ROW_BTN + 1)
#define ROW_INFO  (ROW_BTN + 2)
#define X_BTN1        (FRAME_C0 + 2)   // [  wifi  ]
#define X_BTN2        (FRAME_C0 + 14)  // [ touch ]
#define X_BTN3        (FRAME_C0 + 25)  // [ reiniciar ]
#define X_BTN_SPLIT1  (FRAME_C0 + 13)
#define X_BTN_SPLIT2  (FRAME_C0 + 24)

#define FLASH_US   (150 * 1000)
#define CONFIRM_US (3 * 1000000LL)

enum { IT_BRIGHT, IT_SAVER, IT_SAVER_BRIGHT, IT_WEATHER };

static const char *s_labels[N_ITEMS] = { "brillo", "salvapantallas", "brillo salvap.", "clima cada" };
static const uint8_t SAVER_STEPS[]   = { 0, 1, 2, 3, 5, 10, 15, 30, 60 };
static const uint8_t WEATHER_STEPS[] = { 5, 10, 15, 30, 60 };

static int s_flash_item = -1, s_flash_dir;
static int64_t s_flash_until;
static int s_confirm = -1;  // 0 = wifi, 1 = reiniciar
static int64_t s_confirm_until, s_last_info;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int step_list(const uint8_t *l, int n, int v, int dir)  // valor siguiente/anterior de la lista
{
    int i = 0;
    while (i < n && l[i] < v) i++;  // l[i] >= v
    if (dir > 0) {
        if (i < n && l[i] == v) i++;
    } else {
        i--;
    }
    return l[clampi(i, 0, n - 1)];
}

static void print_center(int c, int r, const char *s, int w, uint16_t fg)
{
    char line[40];
    int len = strlen(s);
    if (len > w) len = w;
    int left = (w - len) / 2;
    snprintf(line, sizeof line, "%*s%-*.*s", left, "", w - left, len, s);
    term_print(c, r, line, fg, COL_BG);
}

static void draw_btn(int c, int r, const char *txt, bool active)
{
    term_print(c, r, txt, active ? COL_BG : COL_MAIN, active ? COL_MAIN : COL_BG);
}

static void draw_items(void)
{
    const settings_t *s = settings_get();
    char buf[16];
    for (int it = 0; it < N_ITEMS; it++) {
        int r = ROW0 + it * ROW_STEP;
        term_print(X_LBL, r, s_labels[it], COL_DIM, COL_BG);
        switch (it) {
        case IT_BRIGHT:
            snprintf(buf, sizeof buf, "%d %%", s->brightness);
            break;
        case IT_SAVER:
            if (s->saver_min) snprintf(buf, sizeof buf, "%d min", s->saver_min);
            else              snprintf(buf, sizeof buf, "apagado");
            break;
        case IT_SAVER_BRIGHT:
            snprintf(buf, sizeof buf, "%d %%", s->saver_bright);
            break;
        default:
            snprintf(buf, sizeof buf, "%d min", s->weather_min);
            break;
        }
        draw_btn(X_MINUS, r, "[-]", s_flash_item == it && s_flash_dir < 0);
        print_center(X_VAL, r, buf, 7, COL_TEXT);
        draw_btn(X_PLUS, r, "[+]", s_flash_item == it && s_flash_dir > 0);
    }
}

static void draw_buttons(void)
{
    bool c0 = s_confirm == 0, c2 = s_confirm == 2;
    term_print(X_BTN1, ROW_BTN, c0 ? "[seguro? ]" : "[  wifi  ]", c0 ? COL_WARN : COL_MAIN, COL_BG);
    term_print(X_BTN2, ROW_BTN, "[ touch ]", COL_MAIN, COL_BG);
    term_print(X_BTN3, ROW_BTN, c2 ? "[  seguro?  ]" : "[ reiniciar ]", c2 ? COL_WARN : COL_MAIN, COL_BG);
}
static void draw_info(void)
{
    char ip[16] = "sin red", line[40];
    esp_netif_ip_info_t info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr) {
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&info.ip));
    }
    snprintf(line, sizeof line, "ip %-15s  v %.12s", ip, esp_app_get_description()->version);
    char pad[40];
    snprintf(pad, sizeof pad, "%-34.34s", line);
    term_print(X_LBL, ROW_INFO, pad, COL_DIM, COL_BG);
}

static void draw_static(void)
{
    for (int c = FRAME_C0 + 1; c < FRAME_C1; c++) {
        bool on = (c - FRAME_C0) % 2;
        term_put(c, ROW_SEP, on ? CH_H : ' ', on ? COL_TRACK : COL_BG, COL_BG);
    }
}

static void reboot_with(const char *msg, bool config_wifi)
{
    settings_save_now();
    term_clear(COL_BG);
    term_print((TERM_COLS - (int)strlen(msg)) / 2, TERM_ROWS / 2, msg, COL_MAIN, COL_BG);
    term_flush();
    vTaskDelay(pdMS_TO_TICKS(800));
    if (config_wifi) prov_request_restart();
    esp_restart();
}

void cfg_enter(void)
{
    s_flash_item = -1;
    s_confirm = -1;
    draw_static();
    draw_items();
    draw_buttons();
    draw_info();
    s_last_info = esp_timer_get_time();
}

void cfg_step(void)
{
    int64_t now = esp_timer_get_time();
    if (s_flash_item >= 0 && now > s_flash_until) {
        s_flash_item = -1;
        draw_items();
    }
    if (s_confirm >= 0 && now > s_confirm_until) {
        s_confirm = -1;
        draw_buttons();
    }
    if (now - s_last_info > 1000000) {
        s_last_info = now;
        draw_info();
    }
}

void cfg_touch(int x, int y)
{
    int64_t now = esp_timer_get_time();
    int col = x / 8;
    int d = y - (ROW0 * 16 + 8);  // distancia al centro de la primera fila de controles
    if (d < -16) return;
    int k = (d + 16) / 32;        // control mas cercano: filas cada 2 celdas = 32 px

    if (k < N_ITEMS) {
        if (col < X_CTRL) return;  // toque sobre la etiqueta
        int dir = (col < X_SPLIT) ? -1 : 1;
        settings_t s = *settings_get();
        switch (k) {
        case IT_BRIGHT:
            s.brightness = clampi(s.brightness + dir * 10, 10, 100);
            break;
        case IT_SAVER:
            s.saver_min = step_list(SAVER_STEPS, sizeof SAVER_STEPS, s.saver_min, dir);
            break;
        case IT_SAVER_BRIGHT:
            s.saver_bright = clampi(s.saver_bright + dir * 5, 5, 100);
            break;
        default:
            s.weather_min = step_list(WEATHER_STEPS, sizeof WEATHER_STEPS, s.weather_min, dir);
            break;
        }
        settings_set(&s);
        s_flash_item = k;
        s_flash_dir = dir;
        s_flash_until = now + FLASH_US;
        draw_items();
    } else if (k == N_ITEMS) {  // fila de botones
        int which = (col < X_BTN_SPLIT1) ? 0 : (col < X_BTN_SPLIT2) ? 1 : 2;
        if (which == 1) {  // calibrar el touch: no es destructivo, sin confirmacion
            touchcal_run();
            pages_redraw();
            return;
        }
        if (s_confirm == which && now < s_confirm_until) {
            if (which == 0) reboot_with("reiniciando en modo config...", true);
            else            reboot_with("reiniciando...", false);
        }
        s_confirm = which;
        s_confirm_until = now + CONFIRM_US;
        draw_buttons();
    }
}
