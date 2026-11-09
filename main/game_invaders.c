#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "term.h"
#include "frame.h"
#include "pages.h"
#include "game_ui.h"
#include "game_invaders.h"

// Region de pixeles: columnas 1..38, filas 2..12 -> 304 x 176 px
#define CV_C0 (FRAME_C0 + 1)
#define CV_R0 (FRAME_R0 + 2)
#define CV_C1 (FRAME_C0 + 38)
#define CV_R1 (FRAME_R0 + 12)
#define CV_X  (CV_C0 * 8)
#define CV_Y  (CV_R0 * 16)
#define CV_W  ((CV_C1 - CV_C0 + 1) * 8)
#define CV_H  ((CV_R1 - CV_R0 + 1) * 16)

#define HUD_R   (FRAME_R0 + 1)
#define MSG_R   (FRAME_R0 + 13)
#define HELP_C  (FRAME_C0 + 14)
#define HELP_W  12
#define SCORE_C (FRAME_C0 + 28)

#define SC        2
#define AR        4
#define AC        7
#define CELL_DX   32
#define CELL_DY   20
#define A_W       (12 * SC)
#define A_H       (8 * SC)
#define GRID_X0   40
#define GRID_Y0   24
#define BUNK_Y    128
#define BUNK_W    11
#define BUNK_H    8
#define INVADE_Y  BUNK_Y          // si llegan a los refugios: game over
#define PLAYER_Y  154
#define PLAYER_W  (13 * SC)
#define PLAYER_H  (8 * SC)
#define GROUND_Y  (CV_H - 2)
#define UFO_Y     4
#define UFO_W     (16 * SC)
#define UFO_H     (7 * SC)

#define PLAYER_SPEED 220.0f
#define PB_SPEED     380.0f
#define AB_SPEED     140.0f
#define UFO_SPEED    70.0f
#define MAX_AB       3
#define MAX_BOOM     4
#define MOVE_ZONE_Y  BUNK_Y   // de los refugios para abajo, el dedo mueve la nave; arriba dispara
#define SUBSTEP_PX   4.0f     // las balas avanzan de a 4 px por chequeo: no atraviesan nada

#define COL_BG     0x0000
#define COL_TEXT   0xDF3F
#define COL_MAIN   0x4BFF
#define COL_DIM    0x6C16
#define COL_ALIEN  0xDF3F
#define COL_PLAYER 0x07E0
#define COL_BUNKER 0x07E0
#define COL_UFO    0xF800
#define COL_SHOT   0xFFFF
#define COL_ASHOT  0xFD80
#define COL_BOOM   0xFD80

typedef struct {
    int w, h;
    const char *const *rows;
} sprite_t;

// ---------- pixel art ----------

static const char *const SQ1[] = { "...##...", "..####..", ".######.", "##.##.##", "########", "..#..#..", ".#.##.#.", "#.#..#.#" };
static const char *const SQ2[] = { "...##...", "..####..", ".######.", "##.##.##", "########", ".#.##.#.", "#......#", ".#....#." };
static const char *const CR1[] = { "..#.....#..", "...#...#...", "..#######..", ".##.###.##.", "###########", "#.#######.#", "#.#.....#.#", "...##.##..." };
static const char *const CR2[] = { "..#.....#..", "#..#...#..#", "#.#######.#", "###.###.###", "###########", ".#########.", "..#.....#..", ".#.......#." };
static const char *const OC1[] = { "....####....", ".##########.", "############", "###..##..###", "############", "...##..##...", "..##.##.##..", "##........##" };
static const char *const OC2[] = { "....####....", ".##########.", "############", "###..##..###", "############", "..###..###..", ".##..##..##.", "..##....##.." };
static const char *const PLAYER[] = { "......#......", ".....###.....", ".....###.....", ".###########.", "#############", "#############", "#############", "#############" };
static const char *const UFO[] = { ".....######.....", "...##########...", "..############..", ".##.##.##.##.##.", "################", "..###..##..###..", "...#........#..." };
static const char *const BOOM[] = { "....#...#...", ".#...#.#..#.", "..#......#..", "...#....#...", "##........##", "...#....#...", "..#..#...#..", ".#...#....#." };
static const char *const BUNKER[BUNK_H] = { "..#######..", ".#########.", "###########", "###########", "###########", "###########", "####...####", "###.....###" };

static const sprite_t SP_SQ1 = { 8, 8, SQ1 }, SP_SQ2 = { 8, 8, SQ2 };
static const sprite_t SP_CR1 = { 11, 8, CR1 }, SP_CR2 = { 11, 8, CR2 };
static const sprite_t SP_OC1 = { 12, 8, OC1 }, SP_OC2 = { 12, 8, OC2 };
static const sprite_t SP_PLAYER = { 13, 8, PLAYER };
static const sprite_t SP_UFO = { 16, 7, UFO };
static const sprite_t SP_BOOM = { 12, 8, BOOM };

static const sprite_t *const ALIEN_SP[3][2] = { { &SP_SQ1, &SP_SQ2 }, { &SP_CR1, &SP_CR2 }, { &SP_OC1, &SP_OC2 } };
static const int ROW_TYPE[AR] = { 0, 1, 1, 2 };
static const int TYPE_PTS[3] = { 30, 20, 10 };

// ---------- estado ----------

typedef enum { S_READY, S_PLAY, S_PAUSE, S_OVER } gstate_t;
typedef struct { float x, y; bool on; } shot_t;
typedef struct { int x, y; int64_t until; } boom_t;

static gstate_t s_state = S_READY;
static bool s_alive[AR][AC];
static int s_alive_n, s_ax, s_ay, s_adir, s_aframe;
static int64_t s_astep_last;
static float s_px;
static bool s_touch_block;            // el toque que arranca no mueve ni dispara
static shot_t s_pb, s_ab[MAX_AB];
static uint8_t s_bunk[3][BUNK_H][BUNK_W];
static boom_t s_boom[MAX_BOOM];
static bool s_ufo_on;
static float s_ufo_x;
static int s_ufo_dir;
static int64_t s_ufo_next;
static int s_lives, s_level;
static uint32_t s_score, s_hi;
static bool s_hi_loaded, s_help, s_redraw;
static int64_t s_last_us, s_next_fire, s_dead_until, s_over_us;

static const char *const HELP_KEYS[] = { "tocar abajo", "tocar arriba", "mantener arriba",
                                         "boton boot", "refugios", "nave roja" };
static const char *const HELP_VALS[] = { "mover la nave", "disparar", "tiro continuo",
                                         "disparar", "se desgastan", "puntos extra" };

// ---------- record ----------

static void load_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "inv_hi", &s_hi);
        nvs_close(h);
    }
    s_hi_loaded = true;
}

static void save_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, "inv_hi", s_hi) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

// ---------- geometria ----------

static int rnd(int n)
{
    return (int)(esp_random() % (uint32_t)n);
}

static int bunk_x(int i)
{
    return 52 + i * 100 - (BUNK_W * SC) / 2;  // centros en 52, 152 y 252
}

static const sprite_t *alien_sp(int r)
{
    return ALIEN_SP[ROW_TYPE[r]][s_aframe];
}

static int alien_x(int r, int c)
{
    return s_ax + c * CELL_DX + (A_W - alien_sp(r)->w * SC) / 2;
}

static int alien_y(int r)
{
    return s_ay + r * CELL_DY;
}

// ---------- dibujo en la capa de pixeles ----------

static void fill_rect(uint16_t *buf, int w, int y0, int h, int x, int y, int rw, int rh, uint16_t col)
{
    for (int yy = y; yy < y + rh; yy++) {
        if (yy < y0 || yy >= y0 + h) continue;
        for (int xx = x; xx < x + rw; xx++) {
            if (xx >= 0 && xx < w) buf[(yy - y0) * w + xx] = col;
        }
    }
}

static void blit(uint16_t *buf, int w, int y0, int h, int sx, int sy, const sprite_t *sp, uint16_t col)
{
    if (sy + sp->h * SC <= y0 || sy >= y0 + h) return;  // fuera de la franja
    for (int j = 0; j < sp->h; j++) {
        const char *row = sp->rows[j];
        for (int i = 0; i < sp->w; i++) {
            if (row[i] == '#') fill_rect(buf, w, y0, h, sx + i * SC, sy + j * SC, SC, SC, col);
        }
    }
}

static void render(uint16_t *buf, int x0, int y0, int w, int h)
{
    (void)x0;
    memset(buf, 0, (size_t)w * h * sizeof(uint16_t));
    int64_t now = esp_timer_get_time();

    fill_rect(buf, w, y0, h, 0, GROUND_Y, w, 1, TERM_RGB(COL_PLAYER));

    uint16_t bc = TERM_RGB(COL_BUNKER);
    for (int i = 0; i < 3; i++) {
        for (int by = 0; by < BUNK_H; by++) {
            for (int bx = 0; bx < BUNK_W; bx++) {
                if (s_bunk[i][by][bx]) fill_rect(buf, w, y0, h, bunk_x(i) + bx * SC, BUNK_Y + by * SC, SC, SC, bc);
            }
        }
    }

    uint16_t ac = TERM_RGB(COL_ALIEN);
    for (int r = 0; r < AR; r++) {
        for (int c = 0; c < AC; c++) {
            if (s_alive[r][c]) blit(buf, w, y0, h, alien_x(r, c), alien_y(r), alien_sp(r), ac);
        }
    }

    if (s_ufo_on) blit(buf, w, y0, h, (int)s_ufo_x, UFO_Y, &SP_UFO, TERM_RGB(COL_UFO));
    if (s_pb.on) fill_rect(buf, w, y0, h, (int)s_pb.x - 1, (int)s_pb.y, 2, 8, TERM_RGB(COL_SHOT));
    for (int i = 0; i < MAX_AB; i++) {
        if (s_ab[i].on) fill_rect(buf, w, y0, h, (int)s_ab[i].x - 1, (int)s_ab[i].y, 2, 8, TERM_RGB(COL_ASHOT));
    }
    if (s_state != S_PLAY || now >= s_dead_until) {
        blit(buf, w, y0, h, (int)s_px - PLAYER_W / 2, PLAYER_Y, &SP_PLAYER, TERM_RGB(COL_PLAYER));
    }
    for (int i = 0; i < MAX_BOOM; i++) {
        if (now < s_boom[i].until) blit(buf, w, y0, h, s_boom[i].x, s_boom[i].y, &SP_BOOM, TERM_RGB(COL_BOOM));
    }
}

// ---------- logica ----------

static void add_boom(int x, int y, int64_t dur_us)
{
    for (int i = 0; i < MAX_BOOM; i++) {
        if (esp_timer_get_time() >= s_boom[i].until) {
            s_boom[i] = (boom_t){ x, y, esp_timer_get_time() + dur_us };
            return;
        }
    }
}

static void reset_bunkers(void)
{
    for (int i = 0; i < 3; i++) {
        for (int y = 0; y < BUNK_H; y++) {
            for (int x = 0; x < BUNK_W; x++) s_bunk[i][y][x] = BUNKER[y][x] == '#';
        }
    }
}

static void reset_wave(int64_t now)
{
    for (int r = 0; r < AR; r++) {
        for (int c = 0; c < AC; c++) s_alive[r][c] = true;
    }
    s_alive_n = AR * AC;
    s_ax = GRID_X0;
    s_ay = GRID_Y0 + (s_level - 1 < 3 ? s_level - 1 : 3) * 8;  // cada nivel arranca un poco mas abajo
    s_adir = 1;
    s_aframe = 0;
    s_pb.on = false;
    for (int i = 0; i < MAX_AB; i++) s_ab[i].on = false;
    s_astep_last = now;
    s_next_fire = now + 1000000;
    s_ufo_on = false;
    s_ufo_next = now + (15 + rnd(10)) * 1000000LL;
}

static void new_game(void)
{
    int64_t now = esp_timer_get_time();
    s_level = 1;
    s_lives = 3;
    s_score = 0;
    s_px = CV_W / 2;
    s_dead_until = 0;
    memset(s_boom, 0, sizeof s_boom);
    reset_bunkers();
    reset_wave(now);
    s_state = S_PLAY;
    s_last_us = now;
}

static void game_over(void)
{
    s_state = S_OVER;
    s_over_us = esp_timer_get_time();
    if (s_score > s_hi) {
        s_hi = s_score;
        save_hi();
    }
}

static int64_t astep_us(void)
{
    int ms = 30 + s_alive_n * 16 - (s_level - 1) * 20;  // menos invasores = mas rapido
    return (ms < 30 ? 30 : ms) * 1000LL;
}

static void step_aliens(void)
{
    int minx = 100000, maxx = -100000, maxy = -1;
    for (int r = 0; r < AR; r++) {
        for (int c = 0; c < AC; c++) {
            if (!s_alive[r][c]) continue;
            int x = alien_x(r, c), w = alien_sp(r)->w * SC;
            if (x < minx) minx = x;
            if (x + w > maxx) maxx = x + w;
            if (alien_y(r) + A_H > maxy) maxy = alien_y(r) + A_H;
        }
    }
    int dx = 4 * s_adir;
    if (minx + dx < 4 || maxx + dx > CV_W - 4) {  // borde: bajan y cambian de sentido
        s_ay += 8;
        maxy += 8;
        s_adir = -s_adir;
    } else {
        s_ax += dx;
    }
    s_aframe ^= 1;
    if (maxy >= INVADE_Y) {
        s_lives = 0;
        game_over();
    }
}

static bool bunker_hit(float fx, float fy)
{
    int x = (int)fx, y = (int)fy;
    if (y < BUNK_Y || y >= BUNK_Y + BUNK_H * SC) return false;
    for (int i = 0; i < 3; i++) {
        if (x < bunk_x(i)) continue;
        int bx = (x - bunk_x(i)) / SC, by = (y - BUNK_Y) / SC;
        if (bx >= BUNK_W || !s_bunk[i][by][bx]) continue;
        for (int dy = -1; dy <= 1; dy++) {  // la celda y algunas vecinas, al azar
            for (int dx = -1; dx <= 1; dx++) {
                int yy = by + dy, xx = bx + dx;
                if (yy < 0 || yy >= BUNK_H || xx < 0 || xx >= BUNK_W) continue;
                if ((dx == 0 && dy == 0) || (esp_random() & 1)) s_bunk[i][yy][xx] = 0;
            }
        }
        return true;
    }
    return false;
}

static void fire(int64_t now)
{
    if (s_pb.on || now < s_dead_until) return;  // un disparo propio a la vez
    s_pb = (shot_t){ s_px, PLAYER_Y - 8, true };
}

static void alien_fire(int64_t now)
{
    int slot = -1;
    for (int i = 0; i < MAX_AB; i++) {
        if (!s_ab[i].on) slot = i;
    }
    if (slot < 0) return;
    for (int t = 0; t < 10; t++) {  // columna al azar con invasores; dispara el de mas abajo
        int c = rnd(AC);
        for (int r = AR - 1; r >= 0; r--) {
            if (!s_alive[r][c]) continue;
            s_ab[slot] = (shot_t){ (float)(alien_x(r, c) + alien_sp(r)->w * SC / 2), (float)(alien_y(r) + A_H), true };
            return;
        }
    }
    (void)now;
}

static void player_hit(int64_t now)
{
    add_boom((int)s_px - PLAYER_W / 2, PLAYER_Y, 1000000);
    for (int i = 0; i < MAX_AB; i++) s_ab[i].on = false;
    s_dead_until = now + 1200000;
    if (--s_lives <= 0) game_over();
}

// Disparo propio en su posicion actual: impactos contra la nave roja, los refugios y los invasores
static void pb_check(void)
{
    if (s_pb.y < 0) {
        s_pb.on = false;
        return;
    }
    if (s_ufo_on && s_pb.x >= s_ufo_x && s_pb.x < s_ufo_x + UFO_W && s_pb.y < UFO_Y + UFO_H) {
        static const int bonus[4] = { 50, 100, 150, 300 };
        s_score += bonus[rnd(4)];
        add_boom((int)s_ufo_x + 8, UFO_Y, 300000);
        s_ufo_on = false;
        s_pb.on = false;
        return;
    }
    if (bunker_hit(s_pb.x, s_pb.y)) {
        s_pb.on = false;
        return;
    }
    for (int r = 0; r < AR; r++) {
        for (int c = 0; c < AC; c++) {
            if (!s_alive[r][c]) continue;
            int ax = alien_x(r, c), ay = alien_y(r), aw = alien_sp(r)->w * SC;
            if (s_pb.x >= ax && s_pb.x < ax + aw && s_pb.y >= ay && s_pb.y < ay + A_H) {
                s_alive[r][c] = false;
                s_alive_n--;
                s_score += TYPE_PTS[ROW_TYPE[r]];
                add_boom(ax, ay, 200000);
                s_pb.on = false;
                return;
            }
        }
    }
}

// Disparo enemigo i en su posicion actual: suelo, refugios y nave
static void ab_check(int i, int64_t now, bool dead)
{
    float tip = s_ab[i].y + 8;
    if (tip >= GROUND_Y) {
        s_ab[i].on = false;
    } else if (bunker_hit(s_ab[i].x, tip)) {
        s_ab[i].on = false;
    } else if (!dead && tip >= PLAYER_Y && s_ab[i].y <= PLAYER_Y + PLAYER_H &&
               fabsf(s_ab[i].x - s_px) < PLAYER_W / 2) {
        player_hit(now);  // apaga todos los disparos enemigos
    }
}

static void update(int64_t now)
{
    float dt = (now - s_last_us) / 1e6f;
    if (dt > 0.05f) dt = 0.05f;
    s_last_us = now;
    bool dead = now < s_dead_until;

    // nave: con el dedo abajo (franja de la nave) se mueve; arriba dispara sin moverse
    int hx, hy;
    bool touching = pages_touch_state(&hx, &hy) && hy / 16 > HUD_R;
    if (s_touch_block && !touching) s_touch_block = false;
    if (touching && !s_touch_block && !dead) {
        if (hy - CV_Y >= MOVE_ZONE_Y) {
            float target = hx - CV_X, d = target - s_px, step = PLAYER_SPEED * dt;
            s_px += (fabsf(d) <= step) ? d : (d > 0 ? step : -step);
        } else {
            fire(now);  // mantener = tiro continuo (igual, un disparo propio a la vez)
        }
    }
    if (s_px < PLAYER_W / 2 + 2) s_px = PLAYER_W / 2 + 2;
    if (s_px > CV_W - PLAYER_W / 2 - 2) s_px = CV_W - PLAYER_W / 2 - 2;

    if (!dead) {  // durante la explosion de la nave, todo espera
        if (now - s_astep_last >= astep_us()) {
            step_aliens();
            s_astep_last = now;
            if (s_state != S_PLAY) return;
        }
        if (now >= s_next_fire) {
            alien_fire(now);
            int base = 600 + rnd(700);
            s_next_fire = now + (int64_t)(base * 1000 / (1.0f + 0.15f * (s_level - 1)));
        }
    }

    // disparo propio, en subpasos: aunque un frame tarde, no atraviesa a nadie
    if (s_pb.on) {
        float dist = PB_SPEED * dt;
        int n = (int)ceilf(dist / SUBSTEP_PX);
        for (int k = 0; k < n && s_pb.on; k++) {
            s_pb.y -= dist / n;
            pb_check();
        }
    }

    // disparos enemigos, tambien en subpasos
    for (int i = 0; i < MAX_AB; i++) {
        if (!s_ab[i].on) continue;
        float dist = AB_SPEED * dt;
        int n = (int)ceilf(dist / SUBSTEP_PX);
        for (int k = 0; k < n && s_ab[i].on; k++) {
            s_ab[i].y += dist / n;
            ab_check(i, now, dead);
        }
        if (s_state != S_PLAY) return;
        dead = now < s_dead_until;  // si recien lo mataron, el resto no lo mata de nuevo
    }

    // nave misteriosa
    if (!s_ufo_on && now >= s_ufo_next) {
        s_ufo_on = true;
        s_ufo_dir = (esp_random() & 1) ? 1 : -1;
        s_ufo_x = (s_ufo_dir > 0) ? -UFO_W : CV_W;
    }
    if (s_ufo_on) {
        s_ufo_x += s_ufo_dir * UFO_SPEED * dt;
        if (s_ufo_x < -UFO_W - 2 || s_ufo_x > CV_W + 2) {
            s_ufo_on = false;
            s_ufo_next = now + (15 + rnd(10)) * 1000000LL;
        }
    }

    if (s_alive_n == 0) {  // oleada limpia
        s_level++;
        reset_wave(now);
    }
}

// ---------- texto (celdas, fuera de la region) ----------

static void draw_text(void)
{
    char b[48];
    term_print(HELP_C, HUD_R, "[como jugar]", COL_MAIN, COL_BG);
    snprintf(b, sizeof b, "pts %05lu", (unsigned long)s_score);
    term_print(SCORE_C, HUD_R, b, COL_TEXT, COL_BG);

    switch (s_state) {
    case S_READY:
        gui_center(FRAME_C0 + 1, MSG_R, 38, "toca para jugar", COL_TEXT);
        break;
    case S_PAUSE:
        gui_center(FRAME_C0 + 1, MSG_R, 38, "pausa - toca para seguir", COL_TEXT);
        break;
    case S_OVER:
        gui_center(FRAME_C0 + 1, MSG_R, 38, "game over - toca para jugar", COL_TEXT);
        break;
    default:
        snprintf(b, sizeof b, "vidas %d   nivel %d   record %lu", s_lives, s_level, (unsigned long)s_hi);
        gui_center(FRAME_C0 + 1, MSG_R, 38, b, COL_DIM);
        break;
    }
}

static void open_help(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = true;
    term_canvas_release();
    gui_help(HELP_KEYS, HELP_VALS, 6);
}

static void close_help(void)
{
    s_help = false;
    gui_clear(GUI_BOX_C0, GUI_BOX_R0, GUI_BOX_C1, GUI_BOX_R1);
    term_canvas_set(CV_C0, CV_R0, CV_C1, CV_R1);
    s_redraw = true;
}

// ---------- interfaz con el menu de juegos ----------

void inv_enter(void)
{
    if (!s_hi_loaded) load_hi();
    if (s_state == S_READY) {  // primera vez: escena armada de fondo
        reset_bunkers();
        reset_wave(esp_timer_get_time());
        s_px = CV_W / 2;
    }
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = false;
    term_canvas_set(CV_C0, CV_R0, CV_C1, CV_R1);
    s_redraw = true;
    inv_step();
}

void inv_leave(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    term_canvas_release();
}

void inv_input(int type, int x, int y)
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
    if (type == PG_DOWN && y / 16 <= HUD_R) return;

    switch (s_state) {
    case S_PLAY:
        if (type == PG_BUTTON) fire(now);  // con el dedo se mueve y dispara en inv_step
        break;
    case S_PAUSE:
        s_state = S_PLAY;
        s_last_us = now;
        s_touch_block = true;
        break;
    case S_OVER:
        if (now - s_over_us < 600000) return;
        new_game();
        s_touch_block = true;
        break;
    default:
        new_game();
        s_touch_block = true;
        break;
    }
    s_redraw = true;
}

void inv_step(void)
{
    if (s_help) return;
    if (s_state == S_PLAY) {
        update(esp_timer_get_time());
        s_redraw = true;
    }
    if (s_redraw) {
        term_canvas_draw(render);
        s_redraw = false;
    }
    draw_text();
}
