#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_err.h"
#include "xpt2046.h"

#define T_SCLK 25
#define T_MOSI 32
#define T_MISO 39
#define T_CS   33

// Calibracion: crudo de 12 bits -> pixeles. Ajustar con el log "touch: ..." de pages.c
#define T_SWAP_XY  1     // el eje crudo X del panel corre a lo alto de la pantalla
#define T_INVERT_X 1
#define T_INVERT_Y 1
#define T_X_MIN    300   // crudo Y (horizontal), medido en las esquinas
#define T_X_MAX    3600
#define T_Y_MIN    700   // crudo X (vertical)
#define T_Y_MAX    3550
#define T_Z_MIN    400

#define SCREEN_W 320
#define SCREEN_H 240

static int xpt_read(uint8_t cmd)  // una conversion completa por bit-bang, SPI modo 0
{
    gpio_set_level(T_CS, 0);

    for (int i = 7; i >= 0; i--) {  // comando, MSB primero
        gpio_set_level(T_MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(T_SCLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(T_SCLK, 0);
    }
    gpio_set_level(T_MOSI, 0);

    int v = 0;
    for (int i = 0; i < 16; i++) {  // 16 clocks: busy + 12 bits + 3 de relleno
        esp_rom_delay_us(1);
        gpio_set_level(T_SCLK, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | gpio_get_level(T_MISO);
        gpio_set_level(T_SCLK, 0);
    }

    gpio_set_level(T_CS, 1);
    return (v >> 3) & 0xFFF;  // mismo formato que antes: 12 bits
}

static int map_axis(int v, int lo, int hi, int out, bool inv)
{
    int r = (v - lo) * out / (hi - lo);
    if (r < 0) r = 0;
    if (r > out - 1) r = out - 1;
    return inv ? out - 1 - r : r;
}

void xpt2046_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << T_SCLK) | (1ULL << T_MOSI) | (1ULL << T_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << T_MISO,  // GPIO39: solo entrada, sin pull internos
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(T_CS, 1);
    gpio_set_level(T_SCLK, 0);
    xpt_read(0xD0);  // power-down entre conversiones, PENIRQ habilitado
}

bool xpt2046_read(xpt2046_point_t *p)
{
    int z1 = xpt_read(0xB1);
    int z2 = xpt_read(0xC1);
    int z = z1 + 4095 - z2;
    if (z < T_Z_MIN) {
        xpt_read(0xD0);
        return false;
    }

    int rx = 0, ry = 0;
    for (int i = 0; i < 4; i++) {
        rx += xpt_read(0xD1);
        ry += xpt_read(0x91);
    }
    xpt_read(0xD0);
    rx /= 4;
    ry /= 4;

    p->raw_x = rx;
    p->raw_y = ry;
    p->z = z;
#if T_SWAP_XY
    int tmp = rx; rx = ry; ry = tmp;
#endif
    p->x = map_axis(rx, T_X_MIN, T_X_MAX, SCREEN_W, T_INVERT_X);
    p->y = map_axis(ry, T_Y_MIN, T_Y_MAX, SCREEN_H, T_INVERT_Y);
    return true;
}
