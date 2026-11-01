#include <stdio.h>
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "term.h"
#include "boot.h"

#define COL_BG     0x0000
#define COL_TEXT   0x07E0
#define COL_HI     0xCFF9
#define COL_OK     0x07E0
#define COL_WARN   0xFFE0
#define STATUS_COL 34
#define BLINK_FRAMES 8      // ~240 ms a 30 ms/frame

typedef struct {
    const char *text;
    uint16_t    color;
    uint8_t     char_frames;   // frames por caracter; 0 = linea de golpe
    uint8_t     pause_frames;  // pausa al terminar la linea
    const char *status;        // etiqueta a la derecha, o NULL
    uint16_t    status_color;
} boot_line_t;

static char s_cpu[40], s_mem[40], s_flash[40];

static const boot_line_t s_lines[] = {
    { "CYD-TERM BIOS v0.1",        COL_HI,   0, 20, NULL,     0 },
    { "",                          COL_TEXT, 0,  5, NULL,     0 },
    { s_cpu,                       COL_TEXT, 0,  6, NULL,     0 },
    { s_mem,                       COL_TEXT, 0,  6, NULL,     0 },
    { s_flash,                     COL_TEXT, 0, 15, NULL,     0 },
    { "",                          COL_TEXT, 0,  5, NULL,     0 },
    { "Iniciando display",         COL_TEXT, 1, 12, "[ OK ]", COL_OK },
    { "Cargando fuente unscii-16", COL_TEXT, 1, 10, "[ OK ]", COL_OK },
    { "Montando /dev/neural",      COL_TEXT, 1, 25, "[ OK ]", COL_OK },
    { "Verificando firewall",      COL_TEXT, 1, 30, "[WARN]", COL_WARN },
    { "Bypass de seguridad",       COL_TEXT, 2, 40, "[ OK ]", COL_OK },
    { "",                          COL_TEXT, 0, 10, NULL,     0 },
    { "> ACCESO CONCEDIDO",        COL_HI,   4, 60, NULL,     0 },
};
#define N_LINES ((int)(sizeof(s_lines) / sizeof(s_lines[0])))
_Static_assert(sizeof(s_lines) / sizeof(s_lines[0]) <= TERM_ROWS, "script mas largo que la pantalla");

typedef enum { ST_TYPING, ST_PAUSE } state_t;

static state_t s_state;
static int s_line, s_col, s_frame;

static void put_cursor(bool on)
{
    term_put(s_col, s_line, on ? 0x80 : ' ', COL_TEXT, COL_BG);
}

void boot_init(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    snprintf(s_cpu, sizeof s_cpu, "CPU:   ESP32 v%d.%d x%d @ %d MHz",
             info.revision / 100, info.revision % 100, info.cores,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    snprintf(s_mem, sizeof s_mem, "MEM:   %lu KB libres",
             (unsigned long)(esp_get_free_heap_size() / 1024));

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(s_flash, sizeof s_flash, "FLASH: %lu MB",
             (unsigned long)(flash_size / (1024 * 1024)));

    term_clear(COL_BG);
    s_state = ST_TYPING;
    s_line = s_col = s_frame = 0;
}

bool boot_step(void)
{
    if (s_line >= N_LINES) return true;
    const boot_line_t *L = &s_lines[s_line];

    switch (s_state) {
    case ST_TYPING:
        if (++s_frame < L->char_frames) break;
        s_frame = 0;
        if (L->text[s_col] == '\0') {
            s_state = ST_PAUSE;
            break;
        }
        do {
            term_put(s_col, s_line, (uint8_t)L->text[s_col], L->color, COL_BG);
            s_col++;
        } while (L->char_frames == 0 && L->text[s_col]);
        put_cursor(true);
        break;

    case ST_PAUSE:
        put_cursor((s_frame / BLINK_FRAMES) % 2 == 0);
        if (++s_frame < L->pause_frames) break;
        put_cursor(false);
        if (L->status) {
            term_print(STATUS_COL, s_line, L->status, L->status_color, COL_BG);
        }
        s_line++;
        s_col = 0;
        s_frame = 0;
        s_state = ST_TYPING;
        break;
    }
    return false;
}
