#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "term.h"
#include "xpt2046.h"
#include "touch_cal.h"

#define NVS_NS  "touch"
#define NVS_KEY "cal1"

#define COL_BG    0x0000
#define COL_TEXT  0xF77B
#define COL_DIM   0x8BA9
#define COL_CROSS 0xFFFF
#define COL_MARK  0xFD80
#define COL_WARN  0xFA08

#define CH_H 0x88  // ─
#define CH_V 0x89  // │

static const char *TAG = "touchcal";
static const int TC[4][2] = { {3, 1}, {36, 1}, {36, 13}, {3, 13} };  // celdas: sup-izq, sup-der, inf-der, inf-izq

static int px(int c) { return c * 8 + 4; }   // centro de la celda en pixeles
static int py(int r) { return r * 16 + 8; }

bool touchcal_load(void)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        xpt2046_cal_t c;
        size_t len = sizeof c;
        if (nvs_get_blob(h, NVS_KEY, &c, &len) == ESP_OK && len == sizeof c) {
            xpt2046_set_cal(&c);
            ok = true;
        }
        nvs_close(h);
    }
    return ok;
}

static void save(const xpt2046_cal_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY, c, sizeof *c) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

static void center_text(int r, const char *s, uint16_t fg)
{
    term_print((TERM_COLS - (int)strlen(s)) / 2, r, s, fg, COL_BG);
}

static void draw_cross(int c, int r)
{
    term_put(c, r, '+', COL_CROSS, COL_BG);
    term_put(c - 1, r, CH_H, COL_CROSS, COL_BG);
    term_put(c + 1, r, CH_H, COL_CROSS, COL_BG);
    term_put(c, r - 1, CH_V, COL_CROSS, COL_BG);
    term_put(c, r + 1, CH_V, COL_CROSS, COL_BG);
}

// Espera un contacto completo y devuelve el crudo promedio, sin las primeras lecturas
static void wait_touch(int *rx, int *ry)
{
    for (;;) {
        long sx = 0, sy = 0;
        int n = 0, seen = 0, up = 0, x, y;
        while (!xpt2046_read_raw(&x, &y)) vTaskDelay(pdMS_TO_TICKS(20));
        for (;;) {
            if (xpt2046_read_raw(&x, &y)) {
                up = 0;
                if (++seen > 3) {  // descarta el arranque del contacto
                    sx += x;
                    sy += y;
                    n++;
                }
            } else if (++up >= 3) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (n >= 5) {  // un toque demasiado breve no sirve: se espera otro
            *rx = (int)(sx / n);
            *ry = (int)(sy / n);
            return;
        }
    }
}

static void test_phase(void)
{
    term_clear(COL_BG);
    center_text(6, "listo: proba tocando la pantalla", COL_TEXT);
    term_flush();
    int64_t end = esp_timer_get_time() + 6000000;
    int last_s = -1;
    while (esp_timer_get_time() < end) {
        xpt2046_point_t p;
        if (xpt2046_read(&p)) term_put(p.x / 8, p.y / 16, '+', COL_MARK, COL_BG);
        int s = (int)((end - esp_timer_get_time()) / 1000000) + 1;
        if (s != last_s) {
            char m[32];
            snprintf(m, sizeof m, "vuelve en %d s ", s);
            center_text(8, m, COL_DIM);
            last_s = s;
        }
        term_flush();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void touchcal_run(void)
{
    for (;;) {
        int rx[4], ry[4];
        for (int i = 0; i < 4; i++) {
            char msg[40];
            term_clear(COL_BG);
            center_text(6, "calibracion del touch", COL_TEXT);
            snprintf(msg, sizeof msg, "toca el centro de la cruz (%d/4)", i + 1);
            center_text(8, msg, COL_DIM);
            draw_cross(TC[i][0], TC[i][1]);
            term_flush();
            wait_touch(&rx[i], &ry[i]);
            ESP_LOGI(TAG, "punto %d: crudo %d,%d", i + 1, rx[i], ry[i]);
        }

        float xl = (rx[0] + rx[3]) / 2.0f, xr = (rx[1] + rx[2]) / 2.0f;
        float yt = (ry[0] + ry[1]) / 2.0f, yb = (ry[2] + ry[3]) / 2.0f;
        if (fabsf(xr - xl) > 500 && fabsf(yb - yt) > 500) {
            xpt2046_cal_t c;
            c.ax = (px(TC[1][0]) - px(TC[0][0])) / (xr - xl);
            c.bx = px(TC[0][0]) - c.ax * xl;
            c.ay = (py(TC[2][1]) - py(TC[0][1])) / (yb - yt);
            c.by = py(TC[0][1]) - c.ay * yt;
            xpt2046_set_cal(&c);
            save(&c);
            ESP_LOGI(TAG, "calibrado: ax=%.4f bx=%.1f ay=%.4f by=%.1f", c.ax, c.bx, c.ay, c.by);
            break;
        }

        term_clear(COL_BG);
        center_text(7, "lecturas invalidas, de nuevo", COL_WARN);
        term_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    test_phase();
}
