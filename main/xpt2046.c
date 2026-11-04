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

static xpt2046_cal_t s_cal;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void default_cal(void)  // equivalente a la calibracion fija anterior
{
    float sx = (float)SCREEN_W / (T_X_MAX - T_X_MIN);
    float sy = (float)SCREEN_H / (T_Y_MAX - T_Y_MIN);
    s_cal.ax = T_INVERT_X ? -sx : sx;
    s_cal.bx = T_INVERT_X ? (SCREEN_W - 1) + sx * T_X_MIN : -sx * T_X_MIN;
    s_cal.ay = T_INVERT_Y ? -sy : sy;
    s_cal.by = T_INVERT_Y ? (SCREEN_H - 1) + sy * T_Y_MIN : -sy * T_Y_MIN;
}

void xpt2046_set_cal(const xpt2046_cal_t *c)
{
    s_cal = *c;
}

// Lectura cruda: presion + promedio de 4 posiciones, con el intercambio de ejes aplicado
static bool sample(int *rx_out, int *ry_out, int *z_out)
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
#if T_SWAP_XY
    int tmp = rx; rx = ry; ry = tmp;
#endif
    *rx_out = rx;
    *ry_out = ry;
    *z_out = z;
    return true;
}

bool xpt2046_read_raw(int *rx, int *ry)
{
    int z;
    return sample(rx, ry, &z);
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
    default_cal();
}

bool xpt2046_read(xpt2046_point_t *p)
{
    int rx, ry, z;
    if (!sample(&rx, &ry, &z)) return false;
    p->raw_x = rx;
    p->raw_y = ry;
    p->z = z;
    p->x = clampi((int)(s_cal.ax * rx + s_cal.bx + 0.5f), 0, SCREEN_W - 1);
    p->y = clampi((int)(s_cal.ay * ry + s_cal.by + 0.5f), 0, SCREEN_H - 1);
    return true;
}
