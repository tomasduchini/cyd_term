#include <stdio.h>
#include <string.h>
#include "term.h"
#include "game_ui.h"

#define COL_BG   0x0000
#define COL_TEXT 0xDF3F
#define COL_MAIN 0x4BFF
#define COL_DIM  0x6C16

#define CH_TL 0x84
#define CH_TR 0x85
#define CH_BL 0x86
#define CH_BR 0x87
#define CH_H  0x88
#define CH_V  0x89

void gui_box(int c0, int r0, int c1, int r1, uint16_t fg)
{
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            uint8_t ch = ' ';
            bool top = r == r0, bot = r == r1, left = c == c0, right = c == c1;
            if (top && left)        ch = CH_TL;
            else if (top && right)  ch = CH_TR;
            else if (bot && left)   ch = CH_BL;
            else if (bot && right)  ch = CH_BR;
            else if (top || bot)    ch = CH_H;
            else if (left || right) ch = CH_V;
            term_put(c, r, ch, ch == ' ' ? COL_BG : fg, COL_BG);
        }
    }
}

void gui_clear(int c0, int r0, int c1, int r1)
{
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) term_put(c, r, ' ', COL_BG, COL_BG);
    }
}

void gui_center(int c, int r, int w, const char *s, uint16_t fg)
{
    char line[48];
    int len = strlen(s);
    if (len > w) len = w;
    int left = (w - len) / 2;
    snprintf(line, sizeof line, "%*s%-*.*s", left, "", w - left, len, s);
    term_print(c, r, line, fg, COL_BG);
}

void gui_help(const char *const keys[], const char *const vals[], int n)
{
    if (n > 6) n = 6;
    gui_box(GUI_BOX_C0, GUI_BOX_R0, GUI_BOX_C1, GUI_BOX_R1, COL_MAIN);
    gui_center(GUI_BOX_C0 + 1, GUI_BOX_R0 + 1, GUI_BOX_C1 - GUI_BOX_C0 - 1, "como jugar", COL_TEXT);
    for (int i = 0; i < n; i++) {
        term_print(GUI_BOX_C0 + 2,  GUI_BOX_R0 + 3 + i, keys[i], COL_DIM, COL_BG);
        term_print(GUI_BOX_C0 + 18, GUI_BOX_R0 + 3 + i, vals[i], COL_TEXT, COL_BG);
    }
    gui_center(GUI_BOX_C0 + 1, GUI_BOX_R1 - 1, GUI_BOX_C1 - GUI_BOX_C0 - 1, "toca para volver", COL_DIM);
}
