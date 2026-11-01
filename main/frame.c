#include <string.h>
#include "term.h"
#include "frame.h"

#define COL_BG     0x0000
#define CH_TL      0x84  // ┌
#define CH_TR      0x85  // ┐
#define CH_BL      0x86  // └
#define CH_BR      0x87  // ┘
#define CH_H       0x88  // ─
#define CH_V       0x89  // │
#define CH_TEE_R   0x8A  // ├
#define CH_TEE_L   0x8B  // ┤
#define CH_DOT_ON  0xF0
#define CH_DOT_OFF 0xF1

static uint16_t s_accent = 0x07E0;
static char s_status[16];
static bool s_status_on;

static uint16_t dim(uint16_t c)   { return (c >> 1) & 0x7BEF; }             // mitad de brillo
static uint16_t light(uint16_t c) { return ((c >> 1) & 0x7BEF) + 0x8410; }  // mezcla con gris claro

static void draw_title(const char *title)  // " // titulo " recortado sobre la linea superior
{
    int c = FRAME_C0 + 2;
    term_put(c++, FRAME_R0, ' ', COL_BG, COL_BG);
    term_print(c, FRAME_R0, "// ", s_accent, COL_BG);
    c += 3;
    term_print(c, FRAME_R0, title, light(s_accent), COL_BG);
    c += strlen(title);
    term_put(c, FRAME_R0, ' ', COL_BG, COL_BG);
}

static void draw_status(void)  // " texto " recortado sobre la linea inferior, a la derecha
{
    int len = strlen(s_status);
    int c = FRAME_C1 - 3 - len;
    term_put(c++, FRAME_R1, ' ', COL_BG, COL_BG);
    term_print(c, FRAME_R1, s_status, s_accent, COL_BG);
    term_put(c + len, FRAME_R1, ' ', COL_BG, COL_BG);
}

void frame_draw(const char *title, uint16_t accent, bool show_status)
{
    s_status_on = show_status;
    s_accent = accent;
    uint16_t line = dim(accent);

    for (int c = FRAME_C0; c <= FRAME_C1; c++) {  // lineas superior e inferior tenues
        term_put(c, FRAME_R0, CH_H, line, COL_BG);
        term_put(c, FRAME_R1, CH_H, line, COL_BG);
    }
    for (int r = FRAME_R0 + 1; r < FRAME_R1; r++) {  // laterales abiertos + interior limpio
        for (int c = FRAME_C0; c <= FRAME_C1; c++) {
            term_put(c, r, ' ', COL_BG, COL_BG);
        }
    }

    // esquinas de HUD en color pleno
    term_put(FRAME_C0,     FRAME_R0,     CH_TL, accent, COL_BG);
    term_put(FRAME_C0 + 1, FRAME_R0,     CH_H,  accent, COL_BG);
    term_put(FRAME_C0,     FRAME_R0 + 1, CH_V,  accent, COL_BG);
    term_put(FRAME_C1,     FRAME_R0,     CH_TR, accent, COL_BG);
    term_put(FRAME_C1 - 1, FRAME_R0,     CH_H,  accent, COL_BG);
    term_put(FRAME_C1,     FRAME_R0 + 1, CH_V,  accent, COL_BG);
    term_put(FRAME_C0,     FRAME_R1,     CH_BL, accent, COL_BG);
    term_put(FRAME_C0 + 1, FRAME_R1,     CH_H,  accent, COL_BG);
    term_put(FRAME_C0,     FRAME_R1 - 1, CH_V,  accent, COL_BG);
    term_put(FRAME_C1,     FRAME_R1,     CH_BR, accent, COL_BG);
    term_put(FRAME_C1 - 1, FRAME_R1,     CH_H,  accent, COL_BG);
    term_put(FRAME_C1,     FRAME_R1 - 1, CH_V,  accent, COL_BG);

    if (title) draw_title(title);
    if (s_status_on && s_status[0]) draw_status();
}

void frame_status(const char *s)
{
    strlcpy(s_status, s, sizeof s_status);
    if (s_status_on) draw_status();
}

void frame_dots(int cur, int n)
{
    int c = (FRAME_C0 + FRAME_C1 + 1 - (2 * n + 1)) / 2;
    term_put(c++, FRAME_R1, ' ', COL_BG, COL_BG);
    for (int i = 0; i < n; i++) {
        term_put(c++, FRAME_R1, i == cur ? CH_DOT_ON : CH_DOT_OFF, s_accent, COL_BG);
        term_put(c++, FRAME_R1, ' ', COL_BG, COL_BG);
    }
}

void frame_hide(void)
{
    s_status_on = false;
}
