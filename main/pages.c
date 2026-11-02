#include <string.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "term.h"
#include "frame.h"
#include "console.h"
#include "pages.h"
#include <stdbool.h>
#include "page_home.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "saver.h"
#include "page_wifi.h"
#include "page_mon.h"
#include "page_pc.h"

#define BTN_GPIO        GPIO_NUM_0
#define BTN_LOCK_FRAMES 6   // ~180 ms sin tocar el boton para habilitar otro cambio

typedef struct {
    const char *title;
    uint16_t accent;
    void (*enter)(void);
    void (*step)(void);
    void (*leave)(void);
    bool status;
} page_t;

static const page_t s_pages[] = {
    { "inicio",  0x07FF, home_enter, home_step, NULL,      false },  // cian
    { "pc",      0xFD80, pc_enter,   pc_step,   NULL,      true  },  // ambar
    { "wifi",    0xE21F, wifi_enter, wifi_step, NULL,      true  },  // magenta
    { "monitor", 0x3EF0, mon_enter,  mon_step,  mon_leave, true  },  // verde
};
#define N_PAGES      ((int)(sizeof s_pages / sizeof s_pages[0]))
#define PAGE_HOME    0
#define PAGE_MONITOR 3

static int s_cur;
static int s_btn_prev = 1;
static int s_btn_lock;
static bool s_saver;
static int64_t s_last_input_us;

static void enter(int i)
{
    s_cur = i;
    frame_draw(s_pages[i].title, s_pages[i].accent, s_pages[i].status);
    frame_dots(i, N_PAGES);
    if (s_pages[i].enter) s_pages[i].enter();
}

void pages_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    s_last_input_us = esp_timer_get_time();
    enter(PAGE_MONITOR);
}

static bool button_pressed(void)  // flanco de bajada con antirrebote
{
    int lvl = gpio_get_level(BTN_GPIO);
    bool pressed = (lvl == 0 && s_btn_prev == 1 && s_btn_lock == 0);
    if (lvl == 0) {
        s_btn_lock = BTN_LOCK_FRAMES;
    } else if (s_btn_lock > 0) {
        s_btn_lock--;
    }
    s_btn_prev = lvl;
    return pressed;
}

void pages_step(void)
{
    bool pressed = button_pressed();
    int64_t now = esp_timer_get_time();
    if (pressed) s_last_input_us = now;

    if (s_saver) {
        if (con_take_event()) s_cur = PAGE_HOME;  // al despertar, va a inicio
        if (pressed) {                            // el toque solo despierta
            s_saver = false;
            saver_exit();
            enter(s_cur);
        } else {
            saver_step();
        }
        return;
    }

    if (con_take_event() && s_cur != PAGE_HOME) {
        if (s_pages[s_cur].leave) s_pages[s_cur].leave();
        enter(PAGE_HOME);
    }
    if (pressed) {
        if (s_pages[s_cur].leave) s_pages[s_cur].leave();
        enter((s_cur + 1) % N_PAGES);
    }
    if (s_pages[s_cur].step) s_pages[s_cur].step();

#if CONFIG_TERM_SAVER_TIMEOUT_S > 0
    if (now - s_last_input_us > (int64_t)CONFIG_TERM_SAVER_TIMEOUT_S * 1000000) {
        if (s_pages[s_cur].leave) s_pages[s_cur].leave();
        s_saver = true;
        saver_enter();
    }
#endif
}
