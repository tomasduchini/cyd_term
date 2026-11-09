#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "pages.h"
#include "game_tetris.h"
#include "game_snake.h"
#include "game_dino.h"
#include "game_invaders.h"
#include "games.h"

#define COL_BG   0x0000
#define COL_MAIN 0x4BFF
#define COL_DIM  0x6C16
#define COL_OFF  0x3186
#define ACCENT   0x4BFF

#define MENU_C    (FRAME_C0 + 12)
#define MENU_W    16
#define MENU_R0   (FRAME_R0 + 3)
#define MENU_STEP 2
#define TOAST_US  (1500 * 1000)

typedef struct {
    const char *name;
    void (*enter)(void);
    void (*step)(void);
    void (*input)(int type, int x, int y);
    void (*leave)(void);
} game_t;

static const game_t s_games[] = {
    { "tetris",    tetris_enter, tetris_step, tetris_input, tetris_leave },
    { "snake",     snake_enter,  snake_step,  snake_input,  snake_leave  },
    { "dino",      dino_enter,   dino_step,   dino_input,   dino_leave   },
    { "invasores", inv_enter,    inv_step,    inv_input,    inv_leave    },
};
#define N_GAMES ((int)(sizeof s_games / sizeof s_games[0]))

static int s_game = -1;  // juego abierto; -1 = menu
static int s_toast = -1;
static int64_t s_toast_until;

static void draw_item(int i)
{
    char line[24];
    bool ready = s_games[i].enter != NULL;
    const char *txt = (s_toast == i) ? "pronto" : s_games[i].name;
    int len = strlen(txt), left = (MENU_W - 2 - len) / 2;
    snprintf(line, sizeof line, "[%*s%-*s]", left, "", MENU_W - 2 - left, txt);
    term_print(MENU_C, MENU_R0 + i * MENU_STEP, line, ready ? COL_MAIN : COL_OFF, COL_BG);
}

static void draw_menu(void)
{
    for (int i = 0; i < N_GAMES; i++) draw_item(i);
}

static void show_menu(void)
{
    frame_draw("juegos", ACCENT, true);
    term_print(FRAME_C0 + 2, FRAME_R0 + 1, "< volver", COL_DIM, COL_BG);
    draw_menu();
}

static void close_game(void)
{
    if (s_game < 0) return;
    if (s_games[s_game].leave) s_games[s_game].leave();
    pages_set_capture(NULL);
    s_game = -1;
}

static void capture(int type, int x, int y)  // todo lo del juego pasa por aca
{
    if (type == PG_DOWN && y / 16 <= FRAME_R0 + 1 && x / 8 < FRAME_C0 + 14) {  // "< volver"
        close_game();
        show_menu();
        return;
    }
    s_games[s_game].input(type, x, y);
}

static void open_game(int i)
{
    s_game = i;
    frame_draw(s_games[i].name, ACCENT, true);
    term_print(FRAME_C0 + 2, FRAME_R0 + 1, "< volver", COL_DIM, COL_BG);
    pages_set_capture(capture);
    s_games[i].enter();
}

void games_enter(void)
{
    s_game = -1;
    s_toast = -1;
    draw_menu();
}

void games_step(void)
{
    if (s_game >= 0) {
        s_games[s_game].step();
        return;
    }
    if (s_toast >= 0 && esp_timer_get_time() > s_toast_until) {
        int i = s_toast;
        s_toast = -1;
        draw_item(i);
    }
}

void games_touch(int x, int y)  // solo en el menu: dentro de un juego manda la captura
{
    int row = y / 16, col = x / 8;
    if (col < MENU_C - 2 || col > MENU_C + MENU_W + 1) return;
    int i = (row - MENU_R0 + 1) / MENU_STEP;
    if (row < MENU_R0 - 1 || i < 0 || i >= N_GAMES) return;
    if (s_games[i].enter) {
        open_game(i);
    } else {
        int prev = s_toast;
        s_toast = i;
        s_toast_until = esp_timer_get_time() + TOAST_US;
        if (prev >= 0 && prev != i) draw_item(prev);
        draw_item(i);
    }
}

void games_leave(void)
{
    close_game();
}
