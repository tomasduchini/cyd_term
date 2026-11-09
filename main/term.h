#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define TERM_COLS 40
#define TERM_ROWS 15

void term_init(esp_lcd_panel_handle_t panel);
bool term_on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
void term_clear(uint16_t bg);
void term_put(int col, int row, uint8_t ch, uint16_t fg, uint16_t bg);
void term_set_fg(int col, int row, uint16_t fg);
void term_set_ch(int col, int row, uint8_t ch);
void term_print(int col, int row, const char *s, uint16_t fg, uint16_t bg);
void term_flush(void);
void term_transition_begin(void);         // foto de la pantalla actual (antes de dibujar lo nuevo)
void term_transition_start(int frames);   // foto del contenido nuevo ya dibujado; arranca la animacion
bool term_transition_step(void);          // avanza un frame; false cuando termino

// Capa de pixeles: una region que el renderer de celdas no toca y que el dueño redibuja entera
typedef void (*term_canvas_fn)(uint16_t *buf, int x0, int y0, int w, int h);  // coords relativas a la region
#define TERM_RGB(c) ((uint16_t)((((c) & 0xFFFF) >> 8) | (((c) & 0xFF) << 8)))      // color listo para el buffer

void term_canvas_set(int c0, int r0, int c1, int r1);  // reserva la region (celdas inclusivas)
void term_canvas_release(void);                         // la devuelve a las celdas y la marca para redibujar
void term_canvas_draw(term_canvas_fn fn);               // redibuja la region con fn, en franjas
