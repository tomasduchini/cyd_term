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
#include "esp_log.h"
#include "term.h"
#include "xpt2046.h"

#define BTN_GPIO          GPIO_NUM_0
#define BTN_LOCK_FRAMES   6   // ~180 ms sin tocar el boton para habilitar otro cambio
#define TOUCH_LOCK_FRAMES 6
#define TRANSITION_FRAMES 14  // ~0,4 s

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
static bool s_transition;
static bool s_touch_prev;
static int s_touch_lock;

typedef enum { IN_NONE, IN_NEXT, IN_PREV } input_t;

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
    xpt2046_init();
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

static input_t touch_input(void)  // flanco de toque con antirrebote; mitad izq = anterior
{
    xpt2046_point_t p;
    bool down = xpt2046_read(&p);
    input_t ev = IN_NONE;
    if (down && !s_touch_prev && s_touch_lock == 0) {
        ESP_LOGD("touch", "x=%d y=%d (crudo %d,%d z=%d)", p.x, p.y, p.raw_x, p.raw_y, p.z);
        ev = (p.x < TERM_COLS * 8 / 2) ? IN_PREV : IN_NEXT;
    }
    if (down) {
        s_touch_lock = TOUCH_LOCK_FRAMES;
    } else if (s_touch_lock > 0) {
        s_touch_lock--;
    }
    s_touch_prev = down;
    return ev;
}

static void go_to(int i, bool leave_current)
{
    if (leave_current && s_pages[s_cur].leave) s_pages[s_cur].leave();
    term_transition_begin();
    enter(i);                                  // la pagina nueva queda en las celdas, sin flush
    term_transition_start(TRANSITION_FRAMES);
    term_transition_step();                    // primer frame ya mezclado: sin destello
    s_transition = true;
}

bool pages_busy(void)
{
    return s_transition;
}

void pages_step(void)
{
    if (s_transition) {
        s_transition = term_transition_step();
        return;
    }

    input_t in = button_pressed() ? IN_NEXT : IN_NONE;
    input_t t = touch_input();  // se lee siempre, para mantener el estado del flanco
    if (in == IN_NONE) in = t;

    int64_t now = esp_timer_get_time();
    if (in != IN_NONE) s_last_input_us = now;

    if (s_saver) {
        if (con_take_event()) s_cur = PAGE_HOME;  // al despertar, va a inicio
        if (in != IN_NONE) {                      // cualquier entrada solo despierta
            s_saver = false;
            saver_exit();
            go_to(s_cur, false);                  // el leave ya se hizo al entrar al salvapantallas
        } else {
            saver_step();
        }
        return;
    }

    if (con_take_event() && s_cur != PAGE_HOME) {
        go_to(PAGE_HOME, true);
        return;
    }
    if (in == IN_NEXT) {
        go_to((s_cur + 1) % N_PAGES, true);
        return;
    }
    if (in == IN_PREV) {
        go_to((s_cur + N_PAGES - 1) % N_PAGES, true);
        return;
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
