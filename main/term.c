#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "term.h"
#include "font8x16.h"
#include "esp_random.h"

#define CELL_W 8
#define CELL_H 16
#define NBUF   4
#define SWAP16(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

typedef struct {
    uint8_t  ch;
    uint16_t fg, bg;
    bool     dirty;
} cell_t;

static cell_t cells[TERM_ROWS][TERM_COLS];
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_buf[NBUF];
static int s_buf_idx;
static SemaphoreHandle_t s_buf_sem;

typedef struct {
    uint8_t ch;
    uint16_t fg, bg;
} tcell_t;

static tcell_t s_old[TERM_ROWS][TERM_COLS], s_new[TERM_ROWS][TERM_COLS];
static uint8_t s_reveal[TERM_ROWS][TERM_COLS];  // frame de revelado; 0xFF = no se anima
static int s_tr_frame = -1, s_tr_frames;

IRAM_ATTR bool term_on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_buf_sem) {
        xSemaphoreGiveFromISR(s_buf_sem, &woken);
    }
    return woken == pdTRUE;
}

void term_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    for (int i = 0; i < NBUF; i++) {
        s_buf[i] = heap_caps_malloc(CELL_W * CELL_H * sizeof(uint16_t), MALLOC_CAP_DMA);
        assert(s_buf[i]);
    }
    s_buf_sem = xSemaphoreCreateCounting(NBUF, NBUF);
    assert(s_buf_sem);
    term_clear(0x0000);
}

static void draw_cell(int col, int row)
{
    const cell_t *c = &cells[row][col];

    xSemaphoreTake(s_buf_sem, portMAX_DELAY);
    uint16_t *buf = s_buf[s_buf_idx];
    s_buf_idx = (s_buf_idx + 1) % NBUF;

    const uint8_t *g = font8x16[c->ch];
    uint16_t fg = SWAP16(c->fg), bg = SWAP16(c->bg);
    uint16_t *p = buf;
    for (int y = 0; y < CELL_H; y++) {
        uint8_t bits = g[y];
        for (int x = 0; x < CELL_W; x++) {
            *p++ = (bits & (0x80 >> x)) ? fg : bg;
        }
    }

    int x0 = col * CELL_W, y0 = row * CELL_H;
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + CELL_W, y0 + CELL_H, buf);
}

void term_clear(uint16_t bg)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            cells[r][c] = (cell_t){ .ch = ' ', .fg = 0, .bg = bg, .dirty = true };
        }
    }
}

void term_put(int col, int row, uint8_t ch, uint16_t fg, uint16_t bg)
{
    if (col < 0 || col >= TERM_COLS || row < 0 || row >= TERM_ROWS) return;
    cell_t *c = &cells[row][col];
    if (c->ch != ch || c->fg != fg || c->bg != bg) {
        c->ch = ch;
        c->fg = fg;
        c->bg = bg;
        c->dirty = true;
    }
}

void term_set_fg(int col, int row, uint16_t fg)
{
	if (col < 0 || col >= TERM_COLS || row < 0 || row >= TERM_ROWS) return;
	cell_t *c = &cells[row][col];
	if (c->fg != fg){
		c->fg = fg;
		c->dirty = true;
	}
}

void term_set_ch(int col, int row, uint8_t ch)
{
	if (col < 0 || col >= TERM_COLS || row < 0 || row >= TERM_ROWS) return;
	cell_t *c = &cells[row][col];
	if (c->ch != ch){
		c->ch = ch;
		c->dirty = true;
	}
}

void term_print(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s && col < TERM_COLS) {
        term_put(col++, row, (uint8_t)*s++, fg, bg);
    }
}

void term_flush(void)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            if (cells[r][c].dirty) {
                draw_cell(c, r);
                cells[r][c].dirty = false;
            }
        }
    }
}

void term_transition_begin(void)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            s_old[r][c] = (tcell_t){ cells[r][c].ch, cells[r][c].fg, cells[r][c].bg };
        }
    }
}

void term_transition_start(int frames)
{
    s_tr_frames = frames;
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            const cell_t *n = &cells[r][c];
            const tcell_t *o = &s_old[r][c];
            s_new[r][c] = (tcell_t){ n->ch, n->fg, n->bg };

            bool same = o->ch == n->ch && o->fg == n->fg && o->bg == n->bg;
            bool visible = o->ch != ' ' || n->ch != ' ';
            // ola de izquierda a derecha con borde irregular
            s_reveal[r][c] = (same || !visible) ? 0xFF
                           : (uint8_t)(c * (frames - 4) / TERM_COLS + esp_random() % 4);
        }
    }
    s_tr_frame = 0;
}

bool term_transition_step(void)
{
    if (s_tr_frame < 0) return false;

    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            uint8_t rv = s_reveal[r][c];
            if (rv == 0xFF) continue;
            const tcell_t *t = &s_new[r][c];
            if (s_tr_frame >= rv) {
                term_put(c, r, t->ch, t->fg, t->bg);
            } else if ((s_tr_frame & 1) == 0) {  // el ruido cambia cada 2 frames
                const tcell_t *src = (t->ch != ' ') ? t : &s_old[r][c];
                term_put(c, r, 0x21 + esp_random() % 94, src->fg, src->bg);
            }
        }
    }

    if (++s_tr_frame >= s_tr_frames) {
        s_tr_frame = -1;  // en el ultimo frame ya se revelaron todas
        return false;
    }
    return true;
}
