#include <stdio.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "bigclock.h"
#include "pcstat.h"
#include "page_pc.h"
#include <string.h>

#define COL_BG    0x0000
#define COL_TEXT  0xFF16  // ambar muy claro
#define COL_MAIN  0xFD80  // ambar
#define COL_DIM   0xA3C8  // ambar apagado
#define COL_TRACK 0x4162  // marron oscuro
#define COL_HOT   0xFA08  // rojo

#define CH_H     0x88  // ─
#define CH_VBAR1 0xE0  // barra vertical angosta, 1..8
#define CH_HBAR1 0xE8  // barra horizontal fina, 1..8 px

#define STALE_US (3 * 1000000LL)

// Filas
#define ROW_STAT  (FRAME_R0 + 1)
#define ROW_CPU   (FRAME_R0 + 2)
#define ROW_CPU_H (FRAME_R0 + 3)
#define ROW_GPU   (FRAME_R0 + 5)
#define ROW_GPU_H (FRAME_R0 + 6)
#define ROW_RAM   (FRAME_R0 + 8)
#define ROW_SEP   (FRAME_R0 + 10)
#define ROW_FPS   (FRAME_R0 + 11)  // digitos grandes: 3 filas

// Columnas
#define X_LBL   (FRAME_C0 + 2)
#define X_BAR   (FRAME_C0 + 7)
#define BAR_W   PC_HIST           // la sparkline va justo debajo de la barra
#define X_PCT   (FRAME_C0 + 24)   // 4 caracteres
#define X_EXT   (FRAME_C0 + 29)   // 9 caracteres: temperatura o uso de RAM
#define X_FPS   (FRAME_C0 + 3)    // 3 digitos grandes
#define X_FSPK  (FRAME_C0 + 18)   // sparkline de FPS, 2 celdas de alto
#define X_STAT  (FRAME_C0 + 22)
#define STAT_W  16

static uint32_t s_seq;
static int64_t s_last_draw;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint16_t temp_color(float t)
{
    if (t < 70) return COL_TEXT;
    if (t < 85) return COL_MAIN;
    return COL_HOT;
}

static void hbar(int c, int r, int w, float pct, uint16_t fg)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int px = (int)(pct / 100.0f * w * 8 + 0.5f);
    for (int i = 0; i < w; i++) {
        int fill = px - i * 8;
        if (fill >= 8) {
            term_put(c + i, r, CH_HBAR1 + 7, fg, COL_BG);
        } else if (fill > 0) {
            term_put(c + i, r, CH_HBAR1 + fill - 1, fg, COL_BG);
        } else {
            term_put(c + i, r, CH_HBAR1 + 7, COL_TRACK, COL_BG);
        }
    }
}

static void spark(int c, int r, const float *h, int n, uint16_t fg)  // 1 celda, escala 0..100
{
    for (int i = 0; i < n; i++) {
        if (h[i] < 0) {
            term_put(c + i, r, CH_VBAR1, COL_TRACK, COL_BG);
            continue;
        }
        int lv = clampi(1 + (int)(h[i] / 100.0f * 7.0f), 1, 8);
        term_put(c + i, r, CH_VBAR1 + lv - 1, fg, COL_BG);
    }
}

static void spark2(int c, int r, const float *h, int n, float top, uint16_t fg)  // 2 celdas
{
    for (int i = 0; i < n; i++) {
        if (h[i] < 0) {
            term_put(c + i, r,     ' ', COL_BG, COL_BG);
            term_put(c + i, r + 1, CH_VBAR1, COL_TRACK, COL_BG);
            continue;
        }
        int lv = clampi(1 + (int)(h[i] / top * 15.0f), 1, 16);
        term_put(c + i, r + 1, CH_VBAR1 + (lv > 8 ? 8 : lv) - 1, fg, COL_BG);
        term_put(c + i, r, lv > 8 ? CH_VBAR1 + lv - 9 : ' ', lv > 8 ? fg : COL_BG, COL_BG);
    }
}

static void draw_metric(int r, const char *label, float pct, float temp, bool live)
{
    char buf[24];
    term_print(X_LBL, r, label, COL_DIM, COL_BG);
    hbar(X_BAR, r, BAR_W, pct, live ? COL_MAIN : COL_DIM);

    if (pct < 0) {
        snprintf(buf, sizeof buf, "  --");
    } else {
        snprintf(buf, sizeof buf, "%3d%%", clampi((int)(pct + 0.5f), 0, 100));
    }
    term_print(X_PCT, r, buf, live ? COL_TEXT : COL_DIM, COL_BG);

    if (temp < 0) {
        snprintf(buf, sizeof buf, "%-9s", " --");
    } else {
        snprintf(buf, sizeof buf, "%3dC     ", clampi((int)(temp + 0.5f), 0, 199));
    }
    term_print(X_EXT, r, buf, live && temp >= 0 ? temp_color(temp) : COL_DIM, COL_BG);
}

static void draw_static(void)
{
    term_print(X_FPS + 12, ROW_FPS + 2, "fps", COL_DIM, COL_BG);
}

static void draw_sep(const char *app)
{
    for (int c = FRAME_C0 + 1; c < FRAME_C1; c++) {
        bool on = (c - FRAME_C0) % 2;
        term_put(c, ROW_SEP, on ? CH_H : ' ', on ? COL_TRACK : COL_BG, COL_BG);
    }
    int c = FRAME_C0 + 3;
    if (app[0]) {
        term_print(c, ROW_SEP, " fps: ", COL_DIM, COL_BG);
        c += 6;
        term_print(c, ROW_SEP, app, COL_MAIN, COL_BG);
        c += strlen(app);
        term_put(c, ROW_SEP, ' ', COL_BG, COL_BG);
    } else {
        term_print(c, ROW_SEP, " fps ", COL_DIM, COL_BG);
    }
}

static void draw_all(void)
{
    pc_t p;
    bool have = pcstat_get(&p);
    int64_t now = esp_timer_get_time();
    bool live = have && now - p.t_us < STALE_US;
    char buf[32], line[32];

    // estado
    const char *st = live ? "en vivo" : (have ? "sin datos" : "esperando pc");
    snprintf(line, sizeof line, "%*s", STAT_W, st);
    term_print(X_STAT, ROW_STAT, line, live ? COL_TEXT : COL_DIM, COL_BG);

    // CPU / GPU / RAM
    draw_metric(ROW_CPU, "cpu", p.cpu, p.cput, live);
    spark(X_BAR, ROW_CPU_H, p.cpu_h, PC_HIST, live ? COL_MAIN : COL_DIM);
    draw_metric(ROW_GPU, "gpu", p.gpu, p.gput, live);
    spark(X_BAR, ROW_GPU_H, p.gpu_h, PC_HIST, live ? COL_MAIN : COL_DIM);

    draw_metric(ROW_RAM, "ram", p.ram, PC_NODATA, live);
    if (p.ramu >= 0 && p.ramt > 0) {
        snprintf(buf, sizeof buf, "%.1f/%.0fG", p.ramu, p.ramt);
    } else {
        snprintf(buf, sizeof buf, " --");
    }
    snprintf(line, sizeof line, "%-9.9s", buf);
    term_print(X_EXT, ROW_RAM, line, live ? COL_TEXT : COL_DIM, COL_BG);

    // FPS
    draw_sep(live ? p.app : "");
    bool has_fps = live && p.fps >= 0;
    bignum_draw(X_FPS, ROW_FPS, has_fps ? (int)(p.fps + 0.5f) : -1, 3, has_fps ? COL_TEXT : COL_DIM);

    float top = 60, lo = -1, hi = -1;
    for (int i = 0; i < PC_FPS_HIST; i++) {
        float v = p.fps_h[i];
        if (v < 0) continue;
        if (v > top) top = v;
        if (lo < 0 || v < lo) lo = v;
        if (v > hi) hi = v;
    }
    spark2(X_FSPK, ROW_FPS, p.fps_h, PC_FPS_HIST, top, live ? COL_MAIN : COL_DIM);
    if (hi >= 0) {
        snprintf(buf, sizeof buf, "min %d  max %d", clampi((int)lo, 0, 999), clampi((int)hi, 0, 999));
    } else {
        snprintf(buf, sizeof buf, "sin fps");
    }
    snprintf(line, sizeof line, "%-20.20s", buf);
    term_print(X_FSPK, ROW_FPS + 2, line, COL_DIM, COL_BG);
}

void pc_enter(void)
{
    draw_static();
    draw_all();
    s_last_draw = esp_timer_get_time();
}

void pc_step(void)
{
    pc_t p;
    int64_t now = esp_timer_get_time();
    bool changed = pcstat_get(&p) && p.seq != s_seq;
    if (changed || now - s_last_draw > 500000) {  // tambien refresca para detectar "sin datos"
        s_seq = p.seq;
        s_last_draw = now;
        draw_all();
    }
}
