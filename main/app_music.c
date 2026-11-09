#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "pcstat.h"
#include "app_music.h"

#define COL_BG    0x0000
#define COL_TEXT  0xDF3F
#define COL_MAIN  0x4BFF
#define COL_DIM   0x6C16
#define COL_TRACK 0x1949
#define COL_OFF   0x3186

#define CH_HBAR1 0xE8  // barra horizontal fina, 1..8 px

#define TEXT_C   (FRAME_C0 + 2)
#define TEXT_W   35
#define TITLE_R  (FRAME_R0 + 3)
#define ARTIST_R (FRAME_R0 + 4)
#define APP_R    (FRAME_R0 + 5)
#define BAR_C    (FRAME_C0 + 4)
#define BAR_W    31
#define BAR_R    (FRAME_R0 + 7)
#define TIME_R   (FRAME_R0 + 8)
#define CTRL_R   (FRAME_R0 + 10)
#define BTN1_C   (FRAME_C0 + 7)
#define BTN2_C   (FRAME_C0 + 16)
#define BTN3_C   (FRAME_C0 + 25)
#define SPLIT1   (FRAME_C0 + 15)
#define SPLIT2   (FRAME_C0 + 24)

#define STALE_US   (4 * 1000000LL)
#define SCROLL_US  (350 * 1000)
#define FLASH_US   (150 * 1000)
#define OPT_US     (2 * 1000000LL)

static int s_flash = -1;           // boton que parpadea tras tocarlo
static int64_t s_flash_until;
static int s_opt_st = -1;          // estado optimista tras play/pausa
static uint32_t s_opt_seq;
static int64_t s_opt_until;

static void print_line(int r, const char *s, uint16_t fg, int64_t now)  // centrado o marquesina
{
    char line[48];
    int len = strlen(s);
    if (len <= TEXT_W) {
        int left = (TEXT_W - len) / 2;
        snprintf(line, sizeof line, "%*s%-*s", left, "", TEXT_W - left, s);
    } else {
        int span = len + 4;  // 4 espacios entre vueltas
        int off = (int)((now / SCROLL_US) % span);
        for (int i = 0; i < TEXT_W; i++) {
            int k = (off + i) % span;
            line[i] = (k < len) ? s[k] : ' ';
        }
        line[TEXT_W] = '\0';
    }
    term_print(TEXT_C, r, line, fg, COL_BG);
}

static void fmt_time(char *buf, size_t n, int s)
{
    if (s < 0) s = 0;
    snprintf(buf, n, "%d:%02d", (s / 60) % 1000, s % 60);
}

static void draw_bar(int pos, int dur)
{
    int px = (dur > 0) ? (int)((int64_t)pos * BAR_W * 8 / dur) : 0;
    for (int i = 0; i < BAR_W; i++) {
        int fill = px - i * 8;
        if (fill >= 8)     term_put(BAR_C + i, BAR_R, CH_HBAR1 + 7, COL_MAIN, COL_BG);
        else if (fill > 0) term_put(BAR_C + i, BAR_R, CH_HBAR1 + fill - 1, COL_MAIN, COL_BG);
        else               term_put(BAR_C + i, BAR_R, CH_HBAR1 + 7, COL_TRACK, COL_BG);
    }
    char a[12], b[12], line[48];
    fmt_time(a, sizeof a, pos);
    fmt_time(b, sizeof b, dur);
    snprintf(line, sizeof line, "%-*s%*s", BAR_W / 2, a, BAR_W - BAR_W / 2, b);
    term_print(BAR_C, TIME_R, line, COL_DIM, COL_BG);
}

static void clear_row(int r, int c, int w)
{
    for (int i = 0; i < w; i++) term_put(c + i, r, ' ', COL_BG, COL_BG);
}

static void draw_btn(int c, const char *txt, int idx, bool enabled)
{
    bool fl = (s_flash == idx);
    uint16_t fg = enabled ? (fl ? COL_BG : COL_MAIN) : COL_OFF;
    term_print(c, CTRL_R, txt, fg, fl ? COL_MAIN : COL_BG);
}

static void draw(int64_t now)
{
    pc_media_t m;
    bool live = pcstat_get_media(&m) && now - m.t_us < STALE_US;

    if (!live) {
        print_line(TITLE_R, "sin conexion con la pc", COL_TEXT, now);
        print_line(ARTIST_R, "corre tools/cyd_pc.py", COL_DIM, now);
        print_line(APP_R, "", COL_DIM, now);
        clear_row(BAR_R, BAR_C, BAR_W);
        clear_row(TIME_R, BAR_C, BAR_W);
        draw_btn(BTN1_C, "[ |<< ]", 0, false);
        draw_btn(BTN2_C, "[  >  ]", 1, false);
        draw_btn(BTN3_C, "[ >>| ]", 2, false);
        return;
    }

    int st = m.st;
    if (s_opt_st >= 0 && m.seq == s_opt_seq && now < s_opt_until) st = s_opt_st;  // optimista
    else s_opt_st = -1;

    if (m.st == 0) {
        print_line(TITLE_R, "no se esta reproduciendo nada", COL_DIM, now);
        print_line(ARTIST_R, "", COL_DIM, now);
        print_line(APP_R, "", COL_DIM, now);
        clear_row(BAR_R, BAR_C, BAR_W);
        clear_row(TIME_R, BAR_C, BAR_W);
    } else {
        print_line(TITLE_R, m.title[0] ? m.title : "(sin titulo)", COL_TEXT, now);
        print_line(ARTIST_R, m.artist, COL_DIM, now);
        print_line(APP_R, m.app, COL_DIM, now);
        if (m.dur > 0) {
            int pos = m.pos + ((st == 1) ? (int)((now - m.t_us) / 1000000) : 0);  // interpolado
            if (pos > m.dur) pos = m.dur;
            draw_bar(pos, m.dur);
        } else {  // la app no informa duracion
            clear_row(BAR_R, BAR_C, BAR_W);
            clear_row(TIME_R, BAR_C, BAR_W);
        }
    }

    draw_btn(BTN1_C, "[ |<< ]", 0, true);
    draw_btn(BTN2_C, st == 1 ? "[  || ]" : "[  >  ]", 1, true);
    draw_btn(BTN3_C, "[ >>| ]", 2, true);
}

void mus_enter(void)
{
    s_flash = -1;
    s_opt_st = -1;
    draw(esp_timer_get_time());
}

void mus_step(void)
{
    int64_t now = esp_timer_get_time();
    if (s_flash >= 0 && now > s_flash_until) s_flash = -1;
    draw(now);  // solo se redibujan las celdas que cambian
}

void mus_touch(int x, int y)
{
    int col = x / 8, row = y / 16;
    if (row < CTRL_R - 1 || row > CTRL_R + 1) return;

    int64_t now = esp_timer_get_time();
    pc_media_t m;
    if (!pcstat_get_media(&m) || now - m.t_us >= STALE_US) return;  // sin PC no hay a quien mandarle

    int idx = (col < SPLIT1) ? 0 : (col < SPLIT2) ? 1 : 2;
    if (idx == 0) {
        pcstat_send_cmd("prev");
    } else if (idx == 2) {
        pcstat_send_cmd("next");
    } else {
        pcstat_send_cmd("play");
        int cur = (s_opt_st >= 0) ? s_opt_st : m.st;
        s_opt_st = (cur == 1) ? 2 : 1;
        s_opt_seq = m.seq;
        s_opt_until = now + OPT_US;
    }
    s_flash = idx;
    s_flash_until = now + FLASH_US;
    draw(now);
}
