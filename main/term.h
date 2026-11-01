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
