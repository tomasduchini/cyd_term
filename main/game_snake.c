#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "term.h"
#include "frame.h"
#include "pages.h"
#include "game_ui.h"
#include "game_snake.h"

#define FW  36                    // campo en casilleros de 8x8
#define FH  20
#define N   (FW * FH)
#define FC0 (FRAME_C0 + 2)        // columnas 2..37
#define FR0 (FRAME_R0 + 3)        // filas 3..12 (20 medias filas)
#define HUD_R  (FRAME_R0 + 1)
#define HELP_C (FRAME_C0 + 14)
#define HELP_W 12
#define SCORE_C (FRAME_C0 + 28)

#define COL_BG   0x0000
#define COL_TEXT 0xDF3F
#define COL_MAIN 0x4BFF
#define COL_DIM  0x6C16
#define COL_BODY 0x07E0
#define COL_HEAD 0xAFE5
#define COL_FOOD 0xF800

#define CH_UP 0x8C  // ▀

typedef enum { S_READY, S_PLAY, S_PAUSE, S_OVER } gstate_t;
static const int DX[4] = { 0, 1, 0, -1 };  // arriba, derecha, abajo, izquierda
static const int DY[4] = { -1, 0, 1, 0 };

static gstate_t s_state = S_READY;
static int16_t s_body[N];       // buffer circular de posiciones (y * FW + x)
static uint8_t s_occ[N];        // ocupacion, para detectar choques
static int s_head, s_len, s_dir, s_food;
static int s_queue[2], s_qn;    // giros pendientes
static uint32_t s_score, s_hi;
static bool s_hi_loaded, s_help;
static int s_eaten;
static int64_t s_last_tick;

static const char *const HELP_KEYS[] = { "tocar arriba", "tocar abajo", "tocar izquierda",
                                         "tocar derecha", "boton boot", "comer" };
static const char *const HELP_VALS[] = { "subir", "bajar", "ir a la izq", "ir a la der",
                                         "pausa", "crecer, +10" };

// ---------- record ----------

static void load_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "snake_hi", &s_hi);
        nvs_close(h);
    }
    s_hi_loaded = true;
}

static void save_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, "snake_hi", s_hi) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

// ---------- logica ----------

static bool place_food(void)
{
    if (s_len >= N) return false;
    for (int t = 0; t < 200; t++) {
        int i = esp_random() % N;
        if (!s_occ[i]) {
            s_food = i;
            return true;
        }
    }
    for (int i = 0; i < N; i++) {  // casi lleno: buscar el primero libre
        if (!s_occ[i]) {
            s_food = i;
            return true;
        }
    }
    return false;
}

static void game_over(void)
{
    s_state = S_OVER;
    if (s_score > s_hi) {
        s_hi = s_score;
        save_hi();
    }
}

static void new_game(void)
{
    memset(s_occ, 0, sizeof s_occ);
    s_len = 4;
    s_dir = 1;
    s_qn = 0;
    s_score = 0;
    s_eaten = 0;
    int y = FH / 2, x0 = FW / 2 - 3;
    for (int k = 0; k < s_len; k++) {
        int i = y * FW + x0 + k;
        s_body[k] = i;
        s_occ[i] = 1;
    }
    s_head = s_len - 1;
    place_food();
    s_state = S_PLAY;
    s_last_tick = esp_timer_get_time();
}

static int64_t tick_us(void)
{
    int ms = 150 - s_eaten * 2;
    return (ms < 60 ? 60 : ms) * 1000LL;
}

static void tick(void)
{
    if (s_qn) {
        s_dir = s_queue[0];
        s_queue[0] = s_queue[1];
        s_qn--;
    }
    int h = s_body[s_head];
    int x = h % FW + DX[s_dir], y = h / FW + DY[s_dir];
    if (x < 0 || x >= FW || y < 0 || y >= FH) {
        game_over();
        return;
    }
    int nh = y * FW + x;
    bool eat = (nh == s_food);
    int tail = s_body[(s_head - s_len + 1 + N) % N];
    if (!eat) s_occ[tail] = 0;  // la cola se corre antes de chequear el choque
    if (s_occ[nh]) {
        game_over();
        return;
    }
    s_head = (s_head + 1) % N;
    s_body[s_head] = nh;
    s_occ[nh] = 1;
    if (eat) {
        s_len++;
        s_eaten++;
        s_score += 10;
        if (!place_food()) game_over();  // no queda lugar: gano
    }
}

static int dir_from_touch(int x, int y)
{
    // centro del campo y medidas en pixeles: 288 x 160
    int dx = x - (FC0 * 8 + FW * 4), dy = y - (FR0 * 16 + FH * 4);
    if (abs(dx) * (FH * 4) > abs(dy) * (FW * 4)) return dx > 0 ? 1 : 3;  // compensa la proporcion
    return dy > 0 ? 2 : 0;
}

// ---------- dibujo ----------

static uint16_t color_at(int i)
{
    if (s_state != S_READY && i == s_food) return COL_FOOD;
    if (s_occ[i]) return (i == s_body[s_head]) ? COL_HEAD : COL_BODY;
    return COL_BG;
}

static void draw_field(void)
{
    for (int r = 0; r < FH / 2; r++) {
        for (int x = 0; x < FW; x++) {
            term_put(FC0 + x, FR0 + r, CH_UP, color_at(2 * r * FW + x), color_at((2 * r + 1) * FW + x));
        }
    }
}

static void draw_overlay(void)
{
    char b[48];
    switch (s_state) {
    case S_READY:
        gui_center(FC0, FR0 + 3, FW, "toca para jugar", COL_TEXT);
        snprintf(b, sizeof b, "record %lu", (unsigned long)s_hi);
        gui_center(FC0, FR0 + 5, FW, b, COL_DIM);
        break;
    case S_PAUSE:
        gui_center(FC0, FR0 + 4, FW, "pausa - toca para seguir", COL_TEXT);
        break;
    case S_OVER:
        gui_center(FC0, FR0 + 3, FW, "game over", COL_TEXT);
        snprintf(b, sizeof b, "puntos %lu   record %lu", (unsigned long)s_score, (unsigned long)s_hi);
        gui_center(FC0, FR0 + 5, FW, b, COL_DIM);
        gui_center(FC0, FR0 + 7, FW, "toca para jugar", COL_DIM);
        break;
    default:
        break;
    }
}

static void draw_hud(void)
{
    char b[16];
    term_print(HELP_C, HUD_R, "[como jugar]", COL_MAIN, COL_BG);
    snprintf(b, sizeof b, "pts %-6lu", (unsigned long)s_score);
    term_print(SCORE_C, HUD_R, b, COL_TEXT, COL_BG);
}

static void draw_static(void)
{
    gui_box(FC0 - 1, FR0 - 1, FC0 + FW, FR0 + FH / 2, COL_DIM);
}

static void open_help(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = true;
    gui_help(HELP_KEYS, HELP_VALS, 6);
}

static void close_help(void)
{
    s_help = false;
    gui_clear(GUI_BOX_C0, GUI_BOX_R0, GUI_BOX_C1, GUI_BOX_R1);
    draw_static();
}

// ---------- interfaz con el menu de juegos ----------

void snake_enter(void)
{
    if (!s_hi_loaded) load_hi();
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = false;
    draw_static();
    snake_step();
}

void snake_leave(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
}

void snake_input(int type, int x, int y)
{
    int64_t now = esp_timer_get_time();

    if (s_help) {
        if (type == PG_DOWN || type == PG_BUTTON) close_help();
        return;
    }
    if (type == PG_DOWN && y / 16 <= HUD_R && x / 8 >= HELP_C && x / 8 < HELP_C + HELP_W) {
        open_help();
        return;
    }

    if (type == PG_BUTTON) {  // pausa / seguir / empezar
        if (s_state == S_PLAY) {
            s_state = S_PAUSE;
        } else if (s_state == S_PAUSE) {
            s_state = S_PLAY;
            s_last_tick = now;
        } else {
            new_game();
        }
        return;
    }
    if (type != PG_DOWN || y / 16 <= HUD_R) return;

    if (s_state != S_PLAY) {  // un toque empieza o sigue (sin girar)
        if (s_state == S_PAUSE) {
            s_state = S_PLAY;
            s_last_tick = now;
        } else {
            new_game();
        }
        return;
    }

    int d = dir_from_touch(x, y);
    int last = s_qn ? s_queue[s_qn - 1] : s_dir;
    if (d != last && d != (last + 2) % 4 && s_qn < 2) s_queue[s_qn++] = d;  // sin giros de 180
}

void snake_step(void)
{
    if (s_help) return;
    int64_t now = esp_timer_get_time();
    if (s_state == S_PLAY && now - s_last_tick >= tick_us()) {
        tick();
        s_last_tick = now;
    }
    draw_field();
    draw_overlay();
    draw_hud();
}
