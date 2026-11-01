#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include "term.h"
#include "frame.h"
#include "page_home.h"
#include "weather.h"
#include "bigclock.h"
#include <string.h>

#define COL_BG    0x0000
#define COL_DIGIT 0xCFDF  // cian muy claro: reloj y temperatura
#define COL_MAIN  0x2EDD  // cian: barras y horarios
#define COL_SEC   0x3D36  // cian apagado: segundos, etiquetas, estado
#define COL_DATE  0x3D36
#define COL_TRACK 0x12CC  // cian oscuro: separador y pista de la barra de rango
#define COL_HOT   0xFD80  // ambar

#define CH_H     0x88  // ─
#define CH_VBAR1 0xE0  // barra vertical angosta, niveles 1..8
#define CH_HBAR1 0xE8  // barra horizontal fina, 1..8 px
#define CH_HBARR1 0xF2  // barra horizontal fina alineada a la derecha, 1..7 px
#define CH_MARK   0xF9  // marca de temperatura actual
#define SCALE_MIN 0     // escala fija de la barra de rango, en C
#define SCALE_MAX 40

#define CLK_C    (FRAME_C0 + 3)   // reloj grande
#define CLK_R    (FRAME_R0 + 2)

#define R_C    (FRAME_C0 + 25)    // columna derecha: icono, temperatura, estado
#define R_W    12
#define ICON_C (R_C + 4)          // icono de 4 celdas centrado en la columna

#define SEP_R  (FRAME_R0 + 8)     // separador punteado
#define LBL_R  (FRAME_R0 + 9)     // "prox 12 h" / amanecer
#define BAR_C  (FRAME_C0 + 3)     // 12 barras angostas contiguas
#define BAR_R  (FRAME_R0 + 10)     // fila superior de las barras
#define HRS_R  (FRAME_R0 + 12)    // horas / min-max
#define SUN_C  (FRAME_C0 + 22)    // bloque derecho de abajo
#define SUN_W  15

#define ICON_BASE 0xA0
enum { ICON_SUN, ICON_MOON, ICON_PARTLY, ICON_CLOUD, ICON_FOG, ICON_RAIN, ICON_SNOW, ICON_STORM };

#define C_SUN   0xFE60  // dorado
#define C_MOON  0xEF7D  // blanco calido
#define C_CLOUD 0xDEFB  // gris claro
#define C_FOG   0x8C71  // gris medio
#define C_DROP  0x07FF  // cian

// Color por zona del icono: { tiles arriba-izq, tiles arriba-der, fila de abajo }
static const uint16_t s_icon_col[8][3] = {
    { C_SUN,   C_SUN,   C_SUN   },  // sol
    { C_MOON,  C_MOON,  C_MOON  },  // luna
    { C_SUN,   C_CLOUD, C_CLOUD },  // parcial
    { C_CLOUD, C_CLOUD, C_CLOUD },  // nube
    { C_FOG,   C_FOG,   C_FOG   },  // niebla
    { C_CLOUD, C_CLOUD, C_DROP  },  // lluvia
    { C_CLOUD, C_CLOUD, C_CLOUD },  // nieve
    { C_CLOUD, C_CLOUD, C_SUN   },  // tormenta (rayo dorado)
};

static const char *s_dias[]  = { "domingo", "lunes", "martes", "miercoles",
                                 "jueves", "viernes", "sabado" };
static const char *s_meses[] = { "ene", "feb", "mar", "abr", "may", "jun",
                                 "jul", "ago", "sep", "oct", "nov", "dic" };

static time_t s_last;
static uint32_t s_wseq;

static void print_pad(int c, int r, const char *s, int width, uint16_t fg)
{
    char line[40];
    snprintf(line, sizeof line, "%-*.*s", width, width, s);
    term_print(c, r, line, fg, COL_BG);
}

static uint8_t vbar_glyph(int level)  // 0..8
{
    if (level <= 0) return ' ';
    if (level > 8) level = 8;
    return CH_VBAR1 + level - 1;
}

static void print_center(int c, int r, const char *s, int width, uint16_t fg)
{
    char line[40];
    int len = strlen(s);
    if (len > width) len = width;
    int left = (width - len) / 2;
    snprintf(line, sizeof line, "%*s%-*.*s", left, "", width - left, len, s);
    term_print(c, r, line, fg, COL_BG);
}

static int temp_px(float t)  // posicion en pixeles dentro de la barra de rango
{
    float f = (t - SCALE_MIN) / (float)(SCALE_MAX - SCALE_MIN);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (int)(f * SUN_W * 8 + 0.5f);
}

static int icon_for(int c, bool day)
{
    if (c == 0)                                       return day ? ICON_SUN : ICON_MOON;
    if (c <= 2)                                       return day ? ICON_PARTLY : ICON_CLOUD;
    if (c == 3)                                       return ICON_CLOUD;
    if (c == 45 || c == 48)                           return ICON_FOG;
    if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return ICON_RAIN;
    if ((c >= 71 && c <= 77) || (c >= 85 && c <= 86)) return ICON_SNOW;
    if (c >= 95)                                      return ICON_STORM;
    return ICON_CLOUD;
}

static void draw_icon(int c, int r, int icon)
{
    for (int ty = 0; ty < 2; ty++) {
        for (int tx = 0; tx < 4; tx++) {
            uint16_t fg = ty ? s_icon_col[icon][2] : s_icon_col[icon][tx < 2 ? 0 : 1];
            term_put(c + tx, r + ty, ICON_BASE + icon * 8 + ty * 4 + tx, fg, COL_BG);
        }
    }
}

static void draw_weather(void)
{
    weather_t w;
    char buf[16];

    for (int c = FRAME_C0 + 1; c < FRAME_C1; c++) {  // separador punteado
        bool on = (c - FRAME_C0) % 2;
        term_put(c, SEP_R, on ? CH_H : ' ', on ? COL_TRACK : COL_BG, COL_BG);
    }
    term_print(BAR_C, LBL_R, "prox 12 h", COL_SEC, COL_BG);

    if (!weather_get(&w)) {
        print_center(R_C, CLK_R + 3, "sin datos", R_W, COL_SEC);
        return;
    }

    // columna derecha
    draw_icon(ICON_C, CLK_R, icon_for(w.code, w.is_day));
    snprintf(buf, sizeof buf, "%.1fC", w.temp);
    print_center(R_C, CLK_R + 3, buf, R_W, COL_DIGIT);
    print_center(R_C, CLK_R + 4, weather_text(w.code), R_W, COL_SEC);

    // pronostico: 12 barras angostas de 2 celdas (16 niveles)
    float lo = w.hourly[0], hi = w.hourly[0];
    int hot = 0;
    for (int i = 1; i < WEATHER_HOURS; i++) {
        if (w.hourly[i] < lo) lo = w.hourly[i];
        if (w.hourly[i] > hi) { hi = w.hourly[i]; hot = i; }
    }
    for (int i = 0; i < WEATHER_HOURS; i++) {
        int level = (hi > lo) ? 1 + (int)((w.hourly[i] - lo) / (hi - lo) * 15.0f + 0.5f) : 8;
        uint16_t fg = (i == hot) ? COL_HOT : COL_MAIN;
        term_put(BAR_C + i, BAR_R + 1, vbar_glyph(level), fg, COL_BG);
        term_put(BAR_C + i, BAR_R,     vbar_glyph(level - 8), fg, COL_BG);
    }
    for (int i = 0; i < WEATHER_HOURS; i += 3) {
        snprintf(buf, sizeof buf, "%02d", (w.first_hour + i) % 24);
        term_print(BAR_C + i, HRS_R, buf, COL_SEC, COL_BG);
    }

    // amanecer / atardecer
    term_print(SUN_C,      LBL_R,     "amanecer",  COL_SEC,  COL_BG);
    term_print(SUN_C + 10, LBL_R,     w.sunrise,   COL_MAIN, COL_BG);
    term_print(SUN_C,      LBL_R + 1, "atardecer", COL_SEC,  COL_BG);
    term_print(SUN_C + 10, LBL_R + 1, w.sunset,    COL_MAIN, COL_BG);

    // barra de rango: escala fija, tramo min..max del dia y marca en la temperatura actual
    int a = temp_px(w.t_min), b = temp_px(w.t_max), m = temp_px(w.temp);
    for (int i = 0; i < SUN_W; i++) {
        int c0 = i * 8, c1 = c0 + 8;
        int s0 = a > c0 ? a : c0;          // interseccion del tramo con la celda
        int s1 = b < c1 ? b : c1;
        uint8_t ch = CH_HBAR1 + 7;         // celda completa
        uint16_t fg = COL_MAIN;
        if (s1 <= s0) {
            fg = COL_TRACK;                // fuera del tramo: pista
        } else if (s0 == c0 && s1 < c1) {
            ch = CH_HBAR1 + (s1 - c0) - 1; // el tramo termina dentro de la celda
        } else if (s0 > c0 && s1 == c1) {
            ch = CH_HBARR1 + (c1 - s0) - 1; // el tramo empieza dentro de la celda
        }
        term_put(SUN_C + i, BAR_R + 1, ch, fg, COL_BG);
    }
    int mc = m / 8;
    if (mc > SUN_W - 1) mc = SUN_W - 1;
    term_put(SUN_C + mc, BAR_R + 1, CH_MARK, COL_DIGIT, COL_BG);

    snprintf(buf, sizeof buf, "min %.0fC", w.t_min);
    print_pad(SUN_C, HRS_R, buf, SUN_W, COL_SEC);
    snprintf(buf, sizeof buf, "max %.0fC", w.t_max);
    term_print(SUN_C + SUN_W - strlen(buf), HRS_R, buf, COL_SEC, COL_BG);
}
static void draw(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    bool synced = tm.tm_year + 1900 >= 2025;

    bigclock_draw(CLK_C, CLK_R, &tm, synced, !synced || tm.tm_sec % 2 == 0, COL_DIGIT);

    char buf[24], line[24];
    if (synced) {
        snprintf(buf, sizeof buf, ":%02d", tm.tm_sec);
    } else {
        snprintf(buf, sizeof buf, ":--");
    }
    term_print(CLK_C + 18, CLK_R + 2, buf, COL_SEC, COL_BG);

    if (synced) {
        snprintf(buf, sizeof buf, "%s %02d %s", s_dias[tm.tm_wday], tm.tm_mday, s_meses[tm.tm_mon]);
    } else {
        snprintf(buf, sizeof buf, "sin hora");
    }
    snprintf(line, sizeof line, "%-16s", buf);  // pisa restos de un texto mas largo
    term_print(CLK_C, CLK_R + 4, line, COL_DATE, COL_BG);
}

void home_enter(void)
{
    weather_t w;
    s_last = time(NULL);
    draw(s_last);
    s_wseq = weather_get(&w) ? w.seq : 0;
    draw_weather();
}

void home_step(void)
{
    time_t now = time(NULL);
    if (now != s_last) {
        s_last = now;
        draw(now);
    }

    weather_t w;
    if (weather_get(&w) && w.seq != s_wseq) {
        s_wseq = w.seq;
        draw_weather();
    }
}
