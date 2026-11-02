#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "term.h"
#include "frame.h"
#include "console.h"

#define COL_BG       0x0000
#define CH_CURSOR    0x80
#define BLINK_FRAMES 8
#define QUEUE_SIZE   1024
#define CTRL_CLEAR   0x05
#define CTRL_PAUSE   0x06
#define CTRL_EVENT   0x07
#define PAUSE_FRAMES 33     // ~1 s a 30 ms/frame

#define X0    (FRAME_C0 + 1)
#define Y0    (FRAME_R0 + 8)               // debajo del separador " log " de monitor
#define CON_W (FRAME_C1 - FRAME_C0 - 1)
#define CON_H (FRAME_R1 - Y0)              // 6 filas

static const uint16_t s_palette[] = {
	0x3EF0,  // \x01 texto (verde)
	0xCFFB,  // \x02 resaltado
	0xFD80,  // \x03 warning (ambar)
	0x5CCD,  // \x04 tenue
};

static uint8_t  s_ch[CON_H][CON_W];
static uint16_t s_fg[CON_H][CON_W];
static int s_cx, s_cy;
static uint16_t s_color;
static int s_idle_frames, s_pause_frames;
static bool s_visible;
static bool s_event;
static StreamBufferHandle_t s_queue;

static void cell(int x, int y, uint8_t ch, uint16_t fg)
{
    s_ch[y][x] = ch;
    s_fg[y][x] = fg;
    if (s_visible) term_put(X0 + x, Y0 + y, ch, fg, COL_BG);
}

static void repaint(void)
{
    for (int y = 0; y < CON_H; y++) {
        for (int x = 0; x < CON_W; x++) {
            term_put(X0 + x, Y0 + y, s_ch[y][x], s_fg[y][x], COL_BG);
        }
    }
}

static void put_cursor(bool on)
{
    if (!s_visible) return;
    term_put(X0 + s_cx, Y0 + s_cy, on ? CH_CURSOR : ' ', on ? s_palette[0] : COL_BG, COL_BG);
}

static void clear_all(void)
{
    memset(s_ch, ' ', sizeof s_ch);
    memset(s_fg, 0, sizeof s_fg);
    s_cx = s_cy = 0;
    if (s_visible) repaint();
}

static void scroll(void)
{
    memmove(s_ch[0], s_ch[1], sizeof s_ch[0] * (CON_H - 1));
    memmove(s_fg[0], s_fg[1], sizeof s_fg[0] * (CON_H - 1));
    memset(s_ch[CON_H - 1], ' ', sizeof s_ch[0]);
    memset(s_fg[CON_H - 1], 0, sizeof s_fg[0]);
    if (s_visible) repaint();
}

static void newline(void)
{
    s_cx = 0;
    if (s_cy < CON_H - 1) {
        s_cy++;
    } else {
        scroll();
    }
}

void con_init(void)
{
    if (!s_queue) {
        s_queue = xStreamBufferCreate(QUEUE_SIZE, 1);
        assert(s_queue);
    }
    s_visible = false;
    s_color = s_palette[0];
    s_idle_frames = s_pause_frames = 0;
    clear_all();
}

void con_type(const char *s)
{
    xStreamBufferSend(s_queue, s, strlen(s), 0);  // si no entra, se trunca
}

void con_show(void)
{
    s_visible = true;
    repaint();
}

void con_hide(void)
{
    s_visible = false;
}

void con_step(void)
{
    uint8_t ch;
    while (s_pause_frames == 0 && xStreamBufferReceive(s_queue, &ch, 1, 0) == 1) {
        if (ch >= 1 && ch <= 4) {
            s_color = s_palette[ch - 1];
            continue;
        }
        if (ch == CTRL_PAUSE) {
            s_pause_frames = PAUSE_FRAMES;
            continue;
        }
        if (ch == CTRL_CLEAR) {
            clear_all();
            continue;
        }
        if (ch == CTRL_EVENT) {
            s_event = true;
            continue;
        }
        put_cursor(false);
        if (ch == '\n') {
            newline();
        } else {
            cell(s_cx, s_cy, ch, s_color);
            if (++s_cx >= CON_W) newline();
        }
        put_cursor(true);
        s_idle_frames = 0;
        return;
    }
    if (s_pause_frames > 0) s_pause_frames--;
    put_cursor((s_idle_frames++ / BLINK_FRAMES) % 2 == 0);
}

bool con_take_event(void)
{
    bool e = s_event;
    s_event = false;
    return e;
}
