#include "driver/ledc.h"
#include "esp_err.h"
#include "backlight.h"

#define BL_MODE  LEDC_LOW_SPEED_MODE
#define BL_TIMER LEDC_TIMER_0
#define BL_CH    LEDC_CHANNEL_0
#define BL_RES   LEDC_TIMER_10_BIT
#define BL_FREQ  5000

void backlight_init(int gpio)
{
    ledc_timer_config_t t = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num = gpio,
        .speed_mode = BL_MODE,
        .channel = BL_CH,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = ((1u << 10) - 1) * percent / 100;
    ledc_set_duty(BL_MODE, BL_CH, duty);
    ledc_update_duty(BL_MODE, BL_CH);
}
