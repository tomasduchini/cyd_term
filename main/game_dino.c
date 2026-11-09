#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "term.h"
#include "frame.h"
#include "pages.h"
#include "game_ui.h"
#include "game_dino.h"

// Region de pixeles: columnas 1..38, filas 3..9 -> 304 x 112 px
#define CV_C0 (FRAME_C0 + 1)
#define CV_R0 (FRAME_R0 + 3)
#define CV_C1 (FRAME_C0 + 38)
#define CV_R1 (FRAME_R0 + 9)
#define CV_W  ((CV_C1 - CV_C0 + 1) * 8)
#define CV_H  ((CV_R1 - CV_R0 + 1) * 16)

#define HUD_R  (FRAME_R0 + 1)
#define SCORE_R (FRAME_R0 + 2)
#define MSG_R  (FRAME_R0 + 11)
#define HELP_C (FRAME_C0 + 14)
#define HELP_W 12

#define SC       2                 // escala del pixel art
#define GROUND   (CV_H - 8)        // linea del suelo
#define DINO_X   20
#define DINO_W   (16 * SC)
#define DINO_H   (18 * SC)
#define CACT_W   (8 * SC)
#define CACT_H   (14 * SC)
#define CACT_GAP 18                // separacion entre cactus de un grupo

#define JUMP_V    400.0f           // px/s
#define GRAVITY   1700.0f
#define GRAV_HOLD 900.0f           // manteniendo el dedo, el primer cuarto de segundo
#define HOLD_S    0.25f
#define SPEED0    170.0f
#define SPEED_INC 6.0f             // px/s por segundo
#define SPEED_MAX 420.0f
#define MAX_OBS   4

#define COL_BG     0x0000
#define COL_TEXT   0xDF3F
#define COL_MAIN   0x4BFF
#define COL_DIM    0x6C16
#define COL_DINO   0xDF3F
#define COL_CACTUS 0x3EF0
#define COL_GROUND 0x6C16

typedef struct {
    int w, h;
    const char *const *rows;
} sprite_t;

// ---------- pixel art ----------

#define DINO_TOP \
    "........########", ".......##.######", ".......#########", ".......#########", \
    ".......#####....", ".......########.", "#.....######....", "#....#######....", \
    "##..#########...", "###.########.#..", "############....", ".##########.....", \
    "..#########.....", "...#######......"

static const char *const DINO_A[] = { DINO_TOP,
    "....###.##......", "....##...#......", "....#....##.....", "....##.........." };
static const char *const DINO_B[] = { DINO_TOP,
    "....###.##......", "....#...##......", "....##...#......", ".........##....." };
static const char *const DINO_J[] = { DINO_TOP,
    "....###.##......", "....##..##......", "....#....#......", "....##...##....." };
static const char *const CACTUS[] = {
    "...##...", "..####..", "..####..", "#.####..", "#.####.#", "#.####.#", "#.####.#",
    "######.#", "..######", "..####..", "..####..", "..####..", "..####..", "..####..",
};

static const sprite_t SP_DINO_A = { 16, 18, DINO_A };
static const sprite_t SP_DINO_B = { 16, 18, DINO_B };
static const sprite_t SP_DINO_J = { 16, 18, DINO_J };
static const sprite_t SP_CACTUS = { 8, 14, CACTUS };

// ---------- estado ----------

typedef enum { S_READY, S_PLAY, S_PAUSE, S_OVER } gstate_t;

typedef struct {
    float x;
    int n;   // cactus en el grupo (1..3)
    bool on;
} obs_t;

static gstate_t s_state = S_READY;
static obs_t s_obs[MAX_OBS];
static float s_jy, s_vy, s_jump_t;   // altura del salto, velocidad vertical, momento del salto
static float s_time, s_speed, s_dist;
static float s_next_gap;
static int64_t s_last_us, s_over_us;
static uint32_t s_score, s_hi;
static bool s_hi_loaded, s_help, s_redraw;

static const char *const HELP_KEYS[] = { "tocar", "mantener", "boton boot", "cactus", "velocidad" };
static const char *const HELP_VALS[] = { "saltar", "saltar mas alto", "saltar", "esquivarlos", "sube sola" };

// ---------- record ----------

static void load_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "dino_hi", &s_hi);
        nvs_close(h);
    }
    s_hi_loaded = true;
}

static void save_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, "dino_hi", s_hi) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

// ---------- dibujo en la capa de pixeles ----------

static void blit(uint16_t *buf, int w, int y0, int h, int sx, int sy, const sprite_t *sp, uint16_t col)
{
    for (int j = 0; j < sp->h; j++) {
        int ya = sy + j * SC;
        if (ya + SC <= y0 || ya >= y0 + h) continue;  // fuera de la franja
        const char *row = sp->rows[j];
        for (int i = 0; i < sp->w; i++) {
            if (row[i] != '#') continue;
            for (int dy = 0; dy < SC; dy++) {
                int y = ya + dy - y0;
                if (y < 0 || y >= h) continue;
                for (int dx = 0; dx < SC; dx++) {
                    int x = sx + i * SC + dx;
                    if (x >= 0 && x < w) buf[y * w + x] = col;
                }
            }
        }
    }
}

static void render(uint16_t *buf, int x0, int y0, int w, int h)
{
    (void)x0;
    memset(buf, 0, (size_t)w * h * sizeof(uint16_t));  // fondo negro

    // suelo y piedritas que se desplazan con la distancia
    uint16_t gc = TERM_RGB(COL_GROUND);
    int off = (int)s_dist;
    for (int x = 0; x < w; x++) {
        if (GROUND >= y0 && GROUND < y0 + h) buf[(GROUND - y0) * w + x] = gc;
        if ((x + off) % 41 == 0 && GROUND + 3 >= y0 && GROUND + 3 < y0 + h) buf[(GROUND + 3 - y0) * w + x] = gc;
        if ((x + off) % 29 == 0 && GROUND + 6 >= y0 && GROUND + 6 < y0 + h) buf[(GROUND + 6 - y0) * w + x] = gc;
    }

    uint16_t cc = TERM_RGB(COL_CACTUS);
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].on) continue;
        for (int k = 0; k < s_obs[i].n; k++) {
            blit(buf, w, y0, h, (int)s_obs[i].x + k * CACT_GAP, GROUND - CACT_H, &SP_CACTUS, cc);
        }
    }

    const sprite_t *d = &SP_DINO_J;  // quieto o en el aire
    if (s_state == S_PLAY && s_jy <= 0) d = ((int)(s_dist / 24) % 2) ? &SP_DINO_B : &SP_DINO_A;
    blit(buf, w, y0, h, DINO_X, GROUND - DINO_H - (int)s_jy, d, TERM_RGB(COL_DINO));
}

// ---------- logica ----------

static float rnd01(void)
{
    return (esp_random() & 0xFFFF) / 65535.0f;
}

static float rightmost(void)
{
    float r = -1000;
    for (int i = 0; i < MAX_OBS; i++) {
        if (s_obs[i].on) {
            float e = s_obs[i].x + CACT_W + (s_obs[i].n - 1) * CACT_GAP;
            if (e > r) r = e;
        }
    }
    return r;
}

static void spawn(void)
{
    for (int i = 0; i < MAX_OBS; i++) {
        if (s_obs[i].on) continue;
        s_obs[i].on = true;
        s_obs[i].x = CV_W + 10;
        s_obs[i].n = 1 + (int)(rnd01() * 2.99f);  // 1 a 3 cactus
        s_next_gap = (260 + rnd01() * 200) * (s_speed / 250.0f);  // mas rapido = mas separados
        return;
    }
}

static void game_over(void)
{
    s_state = S_OVER;
    s_over_us = esp_timer_get_time();
    if (s_score > s_hi) {
        s_hi = s_score;
        save_hi();
    }
    s_redraw = true;
}

static void new_game(void)
{
    memset(s_obs, 0, sizeof s_obs);
    s_jy = s_vy = 0;
    s_time = s_dist = 0;
    s_speed = SPEED0;
    s_score = 0;
    s_next_gap = 200;
    s_state = S_PLAY;
    s_last_us = esp_timer_get_time();
}

static bool collides(void)
{
    // cajas un poco mas chicas que el dibujo: los choques "de raspon" no cuentan
    int dx0 = DINO_X + 6, dx1 = DINO_X + DINO_W - 6;
    int dy1 = GROUND - (int)s_jy - 2;  // pies del dino
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].on) continue;
        int cx0 = (int)s_obs[i].x + 3;
        int cx1 = (int)s_obs[i].x + CACT_W + (s_obs[i].n - 1) * CACT_GAP - 3;
        int cy0 = GROUND - CACT_H + 4;
        if (dx1 > cx0 && dx0 < cx1 && dy1 > cy0) return true;
    }
    return false;
}

static void update(int64_t now)
{
    float dt = (now - s_last_us) / 1e6f;
    if (dt > 0.05f) dt = 0.05f;  // si un frame tardo, no "teletransportar"
    s_last_us = now;

    s_time += dt;
    s_speed = SPEED0 + s_time * SPEED_INC;
    if (s_speed > SPEED_MAX) s_speed = SPEED_MAX;
    s_dist += s_speed * dt;
    s_score = (uint32_t)(s_dist / 20);

    if (s_jy > 0 || s_vy > 0) {  // en el aire
        int hx, hy;
        bool hold = pages_touch_state(&hx, &hy);
        float g = (hold && s_vy > 0 && s_time - s_jump_t < HOLD_S) ? GRAV_HOLD : GRAVITY;
        s_vy -= g * dt;
        s_jy += s_vy * dt;
        if (s_jy <= 0) {
            s_jy = 0;
            s_vy = 0;
        }
    }

    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].on) continue;
        s_obs[i].x -= s_speed * dt;
        if (s_obs[i].x < -80) s_obs[i].on = false;
    }
    if (rightmost() < CV_W - s_next_gap) spawn();

    if (collides()) game_over();
}

static void jump(void)
{
    if (s_jy <= 0) {
        s_vy = JUMP_V;
        s_jump_t = s_time;
    }
}

// ---------- texto (celdas, fuera de la region) ----------

static void draw_text(void)
{
    char b[48];
    term_print(HELP_C, HUD_R, "[como jugar]", COL_MAIN, COL_BG);
    snprintf(b, sizeof b, "puntos %05lu", (unsigned long)s_score);
    term_print(FRAME_C0 + 2, SCORE_R, b, COL_TEXT, COL_BG);
    snprintf(b, sizeof b, "record %05lu", (unsigned long)s_hi);
    term_print(FRAME_C0 + 25, SCORE_R, b, COL_DIM, COL_BG);

    const char *m1 = "", *m2 = "";
    switch (s_state) {
    case S_READY: m1 = "toca o boot para saltar"; m2 = "mantene para saltar mas alto"; break;
    case S_PAUSE: m1 = "pausa"; m2 = "toca para seguir"; break;
    case S_OVER:  m1 = "game over"; m2 = "toca para jugar"; break;
    default: break;
    }
    gui_center(FRAME_C0 + 1, MSG_R, 38, m1, COL_TEXT);
    gui_center(FRAME_C0 + 1, MSG_R + 1, 38, m2, COL_DIM);
}

static void open_help(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = true;
    term_canvas_release();  // el recuadro se dibuja con celdas
    gui_help(HELP_KEYS, HELP_VALS, 5);
}

static void close_help(void)
{
    s_help = false;
    gui_clear(GUI_BOX_C0, GUI_BOX_R0, GUI_BOX_C1, GUI_BOX_R1);
    term_canvas_set(CV_C0, CV_R0, CV_C1, CV_R1);
    s_redraw = true;
}

// ---------- interfaz con el menu de juegos ----------

void dino_enter(void)
{
    if (!s_hi_loaded) load_hi();
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = false;
    term_canvas_set(CV_C0, CV_R0, CV_C1, CV_R1);
    s_redraw = true;
    dino_step();
}

void dino_leave(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    term_canvas_release();
}

void dino_input(int type, int x, int y)
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
    if (type != PG_DOWN && type != PG_BUTTON) return;

    switch (s_state) {
    case S_PLAY:
        jump();
        break;
    case S_PAUSE:
        s_state = S_PLAY;
        s_last_us = now;
        break;
    case S_OVER:
        if (now - s_over_us < 400000) return;  // que el toque del choque no reinicie sin querer
        new_game();
        break;
    default:
        new_game();
        break;
    }
    s_redraw = true;
}

void dino_step(void)
{
    if (s_help) return;
    if (s_state == S_PLAY) {
        update(esp_timer_get_time());
        s_redraw = true;
    }
    if (s_redraw) {  // quieto (listo, pausa, game over): no se redibuja de mas
        term_canvas_draw(render);
        s_redraw = false;
    }
    draw_text();
}
