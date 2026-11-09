#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "term.h"
#include "frame.h"
#include "pages.h"
#include "game_tetris.h"

#define W 10
#define H 20
#define BOARD_C (FRAME_C0 + 15)   // 10 columnas
#define BOARD_R (FRAME_R0 + 2)    // 10 filas = 20 medias filas
#define SIDE_C  (FRAME_C0 + 28)
#define HINT_C  (FRAME_C0 + 2)

#define COL_BG    0x0000
#define COL_TEXT  0xDF3F
#define COL_MAIN  0x4BFF
#define COL_DIM   0x6C16

#define CH_UP 0x8C  // ▀: arriba el color de frente, abajo el de fondo
#define CH_TL 0x84
#define CH_TR 0x85
#define CH_BL 0x86
#define CH_BR 0x87
#define CH_H  0x88
#define CH_V  0x89

#define DAS_US  (180 * 1000)  // demora antes de repetir al mantener
#define ARR_US  (60 * 1000)   // repeticion lateral
#define SOFT_US (40 * 1000)   // bajada rapida
#define HELP_R      (FRAME_R0 + 3)   // fila del boton "como jugar"
#define HELP_BTN_W  12
#define BOX_C0      (FRAME_C0 + 4)   // recuadro de ayuda
#define BOX_C1      (FRAME_C0 + 35)
#define BOX_R0      (FRAME_R0 + 2)
#define BOX_R1      (FRAME_R0 + 12)

// 0 = vacio; 1..7 = I J L O S T Z
static const uint16_t PIECE_COL[8] = { 0x0000, 0x07FF, 0x033F, 0xFC00, 0xFFE0, 0x07E0, 0xB01F, 0xF800 };

// Mascaras 4x4 (bit 15 = esquina superior izquierda), 4 rotaciones en sentido horario
static const uint16_t SHAPES[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },  // I
    { 0x44C0, 0x8E00, 0x6440, 0x0E20 },  // J
    { 0x4460, 0x0E80, 0xC440, 0x2E00 },  // L
    { 0xCC00, 0xCC00, 0xCC00, 0xCC00 },  // O
    { 0x06C0, 0x8C40, 0x6C00, 0x4620 },  // S
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 },  // T
    { 0x0C60, 0x4C80, 0xC600, 0x2640 },  // Z
};

typedef enum { S_READY, S_PLAY, S_PAUSE, S_OVER } gstate_t;
enum { Z_NONE, Z_LEFT, Z_RIGHT, Z_ROT, Z_DOWN };

static gstate_t s_state = S_READY;
static uint8_t s_board[H][W];
static int s_type, s_rot, s_px, s_py, s_next;
static uint8_t s_bag[7];
static int s_bag_n;
static uint32_t s_score, s_lines, s_level = 1, s_hi;
static bool s_hi_loaded;
static int64_t s_last_fall;
static int s_hold_zone;
static int64_t s_hold_t0, s_hold_last;
static bool s_help;  // recuadro "como jugar" abierto
// ---------- record ----------

static void load_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "tetris_hi", &s_hi);
        nvs_close(h);
    }
    s_hi_loaded = true;
}

static void save_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, "tetris_hi", s_hi) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

// ---------- logica ----------

static int next_from_bag(void)  // bolsa de 7: las siete piezas antes de repetir
{
    if (s_bag_n == 0) {
        for (int i = 0; i < 7; i++) s_bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = esp_random() % (i + 1);
            uint8_t t = s_bag[i];
            s_bag[i] = s_bag[j];
            s_bag[j] = t;
        }
        s_bag_n = 7;
    }
    return s_bag[--s_bag_n];
}

static bool fits(int t, int r, int px, int py)
{
    uint16_t m = SHAPES[t][r];
    for (int i = 0; i < 16; i++) {
        if (!(m & (0x8000 >> i))) continue;
        int x = px + i % 4, y = py + i / 4;
        if (x < 0 || x >= W || y >= H) return false;
        if (y >= 0 && s_board[y][x]) return false;
    }
    return true;
}

static bool spawn(void)
{
    s_type = s_next;
    s_next = next_from_bag();
    s_rot = 0;
    s_px = 3;
    s_py = 0;
    return fits(s_type, s_rot, s_px, s_py);
}

static void game_over(void)
{
    s_state = S_OVER;
    s_hold_zone = Z_NONE;
    if (s_score > s_hi) {
        s_hi = s_score;
        save_hi();
    }
}

static void lock_piece(void)
{
    uint16_t m = SHAPES[s_type][s_rot];
    for (int i = 0; i < 16; i++) {
        if (!(m & (0x8000 >> i))) continue;
        int y = s_py + i / 4;
        if (y >= 0) s_board[y][s_px + i % 4] = s_type + 1;
    }

    int cleared = 0;
    for (int y = H - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < W && full; x++) full = s_board[y][x] != 0;
        if (full) {
            memmove(&s_board[1], &s_board[0], (size_t)y * W);  // bajan las filas de arriba
            memset(s_board[0], 0, W);
            cleared++;
            y++;  // revisar de nuevo la misma fila
        }
    }
    static const int pts[5] = { 0, 100, 300, 500, 800 };
    s_score += pts[cleared] * s_level;
    s_lines += cleared;
    s_level = 1 + s_lines / 10;

    if (!spawn()) game_over();
}

static int fall_us(void)
{
    int ms = 800 - ((int)s_level - 1) * 70;
    return (ms < 100 ? 100 : ms) * 1000;
}

static void try_move(int dx)
{
    if (fits(s_type, s_rot, s_px + dx, s_py)) s_px += dx;
}

static void try_rotate(void)
{
    static const int kicks[] = { 0, -1, 1, -2, 2 };  // si no entra, probar corrida
    int r = (s_rot + 1) % 4;
    for (int i = 0; i < 5; i++) {
        if (fits(s_type, r, s_px + kicks[i], s_py)) {
            s_rot = r;
            s_px += kicks[i];
            return;
        }
    }
}

static void step_down(void)
{
    if (fits(s_type, s_rot, s_px, s_py + 1)) s_py++;
    else lock_piece();
}

static void hard_drop(void)
{
    int d = 0;
    while (fits(s_type, s_rot, s_px, s_py + 1)) {
        s_py++;
        d++;
    }
    s_score += d * 2;
    lock_piece();
}

static void new_game(void)
{
    memset(s_board, 0, sizeof s_board);
    s_score = s_lines = 0;
    s_level = 1;
    s_bag_n = 0;
    s_next = next_from_bag();
    spawn();
    s_last_fall = esp_timer_get_time();
}

static int zone_of(int x, int y)
{
    if (x < 104) return Z_LEFT;   // tercio izquierdo
    if (x >= 208) return Z_RIGHT; // tercio derecho
    return (y < BOARD_R * 16 + 80) ? Z_ROT : Z_DOWN;  // centro: arriba rota, abajo baja
}

// ---------- dibujo ----------

static uint8_t cell_at(int x, int y)  // tablero + pieza activa
{
    if (s_state == S_PLAY || s_state == S_PAUSE) {
        int dx = x - s_px, dy = y - s_py;
        if (dx >= 0 && dx < 4 && dy >= 0 && dy < 4 && (SHAPES[s_type][s_rot] & (0x8000 >> (dy * 4 + dx)))) {
            return s_type + 1;
        }
    }
    return s_board[y][x];
}

static void draw_board(void)
{
    for (int r = 0; r < H / 2; r++) {
        for (int x = 0; x < W; x++) {
            uint16_t top = PIECE_COL[cell_at(x, 2 * r)];
            uint16_t bot = PIECE_COL[cell_at(x, 2 * r + 1)];
            term_put(BOARD_C + x, BOARD_R + r, CH_UP, top, bot);
        }
    }
}

static void board_msg(int row, const char *s)  // texto centrado sobre el tablero
{
    char line[16];
    int len = strlen(s), left = (W - len) / 2;
    snprintf(line, sizeof line, "%*s%-*s", left, "", W - left, s);
    term_print(BOARD_C, BOARD_R + row, line, COL_TEXT, COL_BG);
}

static void draw_overlay(void)
{
    switch (s_state) {
    case S_READY: board_msg(3, "toca"); board_msg(4, "para"); board_msg(5, "jugar"); break;
    case S_PAUSE: board_msg(4, "pausa"); board_msg(5, "toca"); break;
    case S_OVER:  board_msg(3, "game"); board_msg(4, "over"); board_msg(6, "toca"); break;
    default: break;
    }
}

static void draw_side(void)
{
    char b[16];
    term_print(SIDE_C, FRAME_R0 + 2, "sig", COL_DIM, COL_BG);
    uint16_t m = SHAPES[s_next][0];
    int y0 = (m & 0xF000) ? 0 : 1;  // si la primera fila esta vacia, mostrar desde la segunda
    for (int x = 0; x < 4; x++) {
        bool t = m & (0x8000 >> (y0 * 4 + x));
        bool u = m & (0x8000 >> ((y0 + 1) * 4 + x));
        uint16_t c = PIECE_COL[s_next + 1];
        term_put(SIDE_C + x, FRAME_R0 + 3, CH_UP, t ? c : 0, u ? c : 0);
    }
    const char *lbl[4] = { "puntos", "lineas", "nivel", "record" };
    uint32_t val[4] = { s_score, s_lines, s_level, s_hi };
    for (int i = 0; i < 4; i++) {
        term_print(SIDE_C, FRAME_R0 + 5 + i * 2, lbl[i], COL_DIM, COL_BG);
        snprintf(b, sizeof b, "%-9lu", (unsigned long)val[i]);
        term_print(SIDE_C, FRAME_R0 + 6 + i * 2, b, COL_TEXT, COL_BG);
    }
}

static void draw_static(void)
{
    int c0 = BOARD_C - 1, c1 = BOARD_C + W, r0 = BOARD_R - 1, r1 = BOARD_R + H / 2;
    term_put(c0, r0, CH_TL, COL_DIM, COL_BG);
    term_put(c1, r0, CH_TR, COL_DIM, COL_BG);
    term_put(c0, r1, CH_BL, COL_DIM, COL_BG);
    term_put(c1, r1, CH_BR, COL_DIM, COL_BG);
    for (int c = c0 + 1; c < c1; c++) {
        term_put(c, r0, CH_H, COL_DIM, COL_BG);
        term_put(c, r1, CH_H, COL_DIM, COL_BG);
    }
    for (int r = r0 + 1; r < r1; r++) {
        term_put(c0, r, CH_V, COL_DIM, COL_BG);
        term_put(c1, r, CH_V, COL_DIM, COL_BG);
    }
    term_print(HINT_C, HELP_R, "[como jugar]", COL_MAIN, COL_BG);
}

static bool in_help_btn(int x, int y)
{
    int c = x / 8, r = y / 16;
    return r >= HELP_R - 1 && r <= HELP_R + 1 && c >= HINT_C && c < HINT_C + HELP_BTN_W;
}

static void draw_help(void)
{
    static const char *keys[] = { "tocar izquierda", "tocar derecha", "mantener",
                                  "centro arriba", "centro abajo", "boton boot" };
    static const char *vals[] = { "mover", "mover", "se repite",
                                  "rotar", "bajar rapido", "caer de golpe" };

    for (int r = BOX_R0; r <= BOX_R1; r++) {
        for (int c = BOX_C0; c <= BOX_C1; c++) {
            uint8_t ch = ' ';
            bool top = r == BOX_R0, bot = r == BOX_R1, left = c == BOX_C0, right = c == BOX_C1;
            if (top && left)        ch = CH_TL;
            else if (top && right)  ch = CH_TR;
            else if (bot && left)   ch = CH_BL;
            else if (bot && right)  ch = CH_BR;
            else if (top || bot)    ch = CH_H;
            else if (left || right) ch = CH_V;
            term_put(c, r, ch, ch == ' ' ? COL_BG : COL_MAIN, COL_BG);
        }
    }
    term_print(BOX_C0 + 11, BOX_R0 + 1, "como jugar", COL_TEXT, COL_BG);
    for (int i = 0; i < 6; i++) {
        term_print(BOX_C0 + 2,  BOX_R0 + 3 + i, keys[i], COL_DIM, COL_BG);
        term_print(BOX_C0 + 18, BOX_R0 + 3 + i, vals[i], COL_TEXT, COL_BG);
    }
    term_print(BOX_C0 + 8, BOX_R1 - 1, "toca para volver", COL_DIM, COL_BG);
}

static void open_help(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_help = true;
    s_hold_zone = Z_NONE;
    draw_help();
}

static void close_help(void)
{
    s_help = false;
    for (int r = BOX_R0; r <= BOX_R1; r++) {  // limpiar el recuadro
        for (int c = BOX_C0; c <= BOX_C1; c++) term_put(c, r, ' ', COL_BG, COL_BG);
    }
    draw_static();  // borde del tablero y boton; el resto se redibuja en cada frame
}

// ---------- interfaz con el menu de juegos ----------

void tetris_enter(void)
{
    s_help = false;
    if (!s_hi_loaded) load_hi();
    if (s_state == S_PLAY) s_state = S_PAUSE;  // al volver, en pausa
    s_hold_zone = Z_NONE;
    draw_static();
    tetris_step();
}

void tetris_leave(void)
{
    if (s_state == S_PLAY) s_state = S_PAUSE;
    s_hold_zone = Z_NONE;
}

void tetris_input(int type, int x, int y)
{
    int64_t now = esp_timer_get_time();

    if (s_help) {  // con la ayuda abierta, un toque o el boton solo la cierran
        if (type == PG_DOWN || type == PG_BUTTON) close_help();
        return;
    }
    if (type == PG_DOWN && in_help_btn(x, y)) {
        open_help();
        return;
    }

    if (s_state != S_PLAY) {  // cualquier toque o el boton: empezar o seguir
        if (type == PG_DOWN || type == PG_BUTTON) {
            if (s_state != S_PAUSE) new_game();
            s_state = S_PLAY;
            s_last_fall = now;
            s_hold_zone = Z_NONE;  // ese toque no mueve la pieza
        }
        return;
    }

    if (type == PG_BUTTON) {
        hard_drop();
        s_last_fall = now;
        return;
    }
    if (type != PG_DOWN) return;

    int z = zone_of(x, y);
    switch (z) {
    case Z_LEFT:  try_move(-1); break;
    case Z_RIGHT: try_move(1); break;
    case Z_ROT:   try_rotate(); break;
    case Z_DOWN:  step_down(); s_last_fall = now; break;
    }
    s_hold_zone = z;
    s_hold_t0 = s_hold_last = now;
}

void tetris_step(void)
{
    int64_t now = esp_timer_get_time();
    if (s_help) return;  // no dibujar encima del recuadro

    if (s_state == S_PLAY) {
        // mantener apretado: repetir movimiento o bajar rapido
        int hx, hy;
        if (s_hold_zone != Z_NONE && pages_touch_state(&hx, &hy) && zone_of(hx, hy) == s_hold_zone) {
            if (s_hold_zone == Z_DOWN) {
                if (now - s_hold_last >= SOFT_US) {
                    step_down();
                    s_hold_last = now;
                    s_last_fall = now;
                }
            } else if (s_hold_zone != Z_ROT && now - s_hold_t0 >= DAS_US && now - s_hold_last >= ARR_US) {
                try_move(s_hold_zone == Z_LEFT ? -1 : 1);
                s_hold_last = now;
            }
        } else {
            s_hold_zone = Z_NONE;
        }

        if (s_state == S_PLAY && now - s_last_fall >= fall_us()) {  // gravedad
            step_down();
            s_last_fall = now;
        }
    }

    draw_board();
    draw_overlay();
    draw_side();
}
