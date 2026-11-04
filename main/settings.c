#include <stdbool.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "backlight.h"
#include "settings.h"

#define NVS_NS  "cfg"
#define NVS_KEY "v1"   // subir si cambia settings_t: arranca con los valores de fabrica
#define SAVE_DELAY_US (2 * 1000000LL)

static settings_t s_cfg;
static bool s_dirty;
static int64_t s_dirty_us;

static void defaults(settings_t *s)  // de fabrica: lo que se puso en menuconfig
{
    s->brightness   = 100;
    s->saver_min    = (CONFIG_TERM_SAVER_TIMEOUT_S + 59) / 60;
    s->saver_bright = CONFIG_TERM_SAVER_BRIGHTNESS;
    s->weather_min  = CONFIG_TERM_WEATHER_PERIOD_MIN;
}

static uint8_t clamp8(int v, int lo, int hi)
{
    return (uint8_t)(v < lo ? lo : (v > hi ? hi : v));
}

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        settings_t tmp;
        size_t len = sizeof tmp;
        if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK && len == sizeof tmp) s_cfg = tmp;
        nvs_close(h);
    }
    // por las dudas, rangos validos aunque lo guardado sea raro
    s_cfg.brightness   = clamp8(s_cfg.brightness, 10, 100);
    s_cfg.saver_min    = clamp8(s_cfg.saver_min, 0, 60);
    s_cfg.saver_bright = clamp8(s_cfg.saver_bright, 5, 100);
    s_cfg.weather_min  = clamp8(s_cfg.weather_min, 1, 120);
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_set(const settings_t *s)
{
    if (s->brightness != s_cfg.brightness) backlight_set(s->brightness);
    s_cfg = *s;
    s_dirty = true;
    s_dirty_us = esp_timer_get_time();
}

void settings_save_now(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof s_cfg) == ESP_OK) nvs_commit(h);
        nvs_close(h);
        ESP_LOGI("cfg", "ajustes guardados");
    }
}

void settings_step(void)
{
    if (s_dirty && esp_timer_get_time() - s_dirty_us >= SAVE_DELAY_US) settings_save_now();
}
