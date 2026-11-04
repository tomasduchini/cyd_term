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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provision.h"
#include "stdlib.h"
#include "settings.h"
#include "page_cfg.h"
#include "touch_cal.h"

#define BTN_GPIO          GPIO_NUM_0
#define SWIPE_MIN_PX    60              // desplazamiento horizontal minimo para deslizar
#define TAP_SLOP_PX     20              // movimiento maximo para que cuente como toque
#define TAP_MAX_US      (600 * 1000)    // duracion maxima de un toque
#define TOUCH_UP_FRAMES 2               // frames sin presion para dar por terminado el contacto
#define BTN_LOCK_FRAMES 6   // ~180 ms sin tocar el boton para habilitar otro cambio
#define TRANSITION_FRAMES 14  // ~0,4 s

typedef struct {
    const char *title;
    uint16_t accent;
    void (*enter)(void);
    void (*step)(void);
    void (*leave)(void);
    bool status;
    void (*touch)(int x, int y);   // toque dentro de la pagina, en pixeles; NULL si no usa
} page_t;

static const page_t s_pages[] = {
    { "inicio",  0x2EDD, home_enter, home_step, NULL,      false },  // cian
    { "pc",      0xFD80, pc_enter,   pc_step,   NULL,      true  },  // ambar
    { "wifi",    0xE21F, wifi_enter, wifi_step, NULL,      true  },  // magenta
    { "monitor", 0x3EF0, mon_enter,  mon_step,  mon_leave, true  },  // verde
    { "config",  0xD549, cfg_enter,  cfg_step,  NULL,      true,  cfg_touch },  // laton
};
#define N_PAGES      ((int)(sizeof s_pages / sizeof s_pages[0]))
#define PAGE_HOME    0
#define PAGE_MONITOR 3

static int s_cur;
static int s_btn_prev = 1;
static int s_btn_lock;
#define BTN_LONG_US (3 * 1000000LL)

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG } btn_ev_t;

static bool s_btn_held, s_btn_long_fired;
static int64_t s_btn_down_us;
static bool s_saver;
static int64_t s_last_input_us;
static bool s_transition;
typedef enum { G_NONE, G_DOWN, G_TAP, G_SWIPE_L, G_SWIPE_R } gesture_type_t;

typedef struct {
    gesture_type_t type;
    int x, y;  // posicion del toque (G_TAP y G_DOWN)
} gesture_t;

static bool s_t_down, s_t_consumed;
static int s_t_x0, s_t_y0, s_t_x, s_t_y, s_t_up;
static long s_t_sx, s_t_sy;
static int s_t_n;
static int64_t s_t_t0;

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
    if (!touchcal_load()) touchcal_run();  // primera vez: calibrar
    enter(PAGE_MONITOR);
}

// Toque corto: evento al soltar. Toque largo: evento al cumplirse BTN_LONG_US, una sola vez.
static btn_ev_t button_event(void)
{
    int lvl = gpio_get_level(BTN_GPIO);
    int64_t now = esp_timer_get_time();
    btn_ev_t ev = BTN_NONE;

    if (lvl == 0 && s_btn_prev == 1 && s_btn_lock == 0) {  // apretado
        s_btn_held = true;
        s_btn_long_fired = false;
        s_btn_down_us = now;
    }
    if (s_btn_held && lvl == 0 && !s_btn_long_fired && now - s_btn_down_us >= BTN_LONG_US) {
        s_btn_long_fired = true;
        ev = BTN_LONG;
    }
    if (s_btn_held && lvl == 1 && s_btn_prev == 0) {  // soltado
        s_btn_held = false;
        if (!s_btn_long_fired) ev = BTN_SHORT;
    }

    if (lvl == 0) {
        s_btn_lock = BTN_LOCK_FRAMES;
    } else if (s_btn_lock > 0) {
        s_btn_lock--;
    }
    s_btn_prev = lvl;
    return ev;
}

// Sigue el contacto y devuelve un evento: G_DOWN al apoyar, y al soltar G_TAP o G_SWIPE_*
static gesture_t gesture_poll(void)
{
    gesture_t g = { G_NONE, 0, 0 };
    xpt2046_point_t p;
    bool touching = xpt2046_read(&p);
    int64_t now = esp_timer_get_time();

    if (touching) {
        s_t_up = 0;
        if (!s_t_down) {  // comienzo del contacto
            s_t_down = true;
            s_t_consumed = false;
            s_t_x0 = s_t_x = p.x;
            s_t_y0 = s_t_y = p.y;
            s_t_sx = p.x;
            s_t_sy = p.y;
            s_t_n = 1;
            s_t_t0 = now;
            g = (gesture_t){ G_DOWN, p.x, p.y };
        } else {
            s_t_x = p.x;
            s_t_y = p.y;
            s_t_sx += p.x;
            s_t_sy += p.y;
            s_t_n++;
        }
        return g;
    }

    if (!s_t_down) return g;
    if (++s_t_up < TOUCH_UP_FRAMES) return g;  // espera: una lectura suelta no corta el gesto
    s_t_down = false;
    if (s_t_consumed) return g;

    int dx = s_t_x - s_t_x0;
    int dy = s_t_y - s_t_y0;
    if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > 2 * abs(dy)) {
        g.type = (dx < 0) ? G_SWIPE_L : G_SWIPE_R;
        ESP_LOGI("touch", "deslizar %s (dx=%d dy=%d)", dx < 0 ? "izquierda" : "derecha", dx, dy);
    } else if (abs(dx) <= TAP_SLOP_PX && abs(dy) <= TAP_SLOP_PX && now - s_t_t0 <= TAP_MAX_US) {
        g = (gesture_t){ G_TAP, (int)(s_t_sx / s_t_n), (int)(s_t_sy / s_t_n) };
        ESP_LOGI("touch", "toque x=%d y=%d (%d lecturas)", g.x, g.y, s_t_n);
    }
    return g;
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
    settings_step();  // guardado diferido de los ajustes
    if (s_transition) {
        s_transition = term_transition_step();
        return;
    }

    btn_ev_t b = button_event();
    if (b == BTN_LONG) {  // reinicio en modo configuracion WiFi
        term_clear(0x0000);
        term_print(5, 7, "reiniciando en modo config...", 0xFD80, 0x0000);
        term_flush();
        vTaskDelay(pdMS_TO_TICKS(800));
        prov_request_restart();
    }
    gesture_t g = gesture_poll();  // siempre, para seguir el contacto aunque no se use

    int64_t now = esp_timer_get_time();
    if (b != BTN_NONE || g.type != G_NONE) s_last_input_us = now;

    if (s_saver) {
        if (con_take_event()) s_cur = PAGE_HOME;  // al despertar, va a inicio
        if (b == BTN_SHORT || g.type == G_DOWN) {  // despierta al apoyar el dedo
            if (g.type == G_DOWN) s_t_consumed = true;  // ese contacto no hace nada mas
            s_saver = false;
            saver_exit();
            go_to(s_cur, false);  // el leave ya se hizo al entrar al salvapantallas
        } else {
            saver_step();
        }
        return;
    }

    if (con_take_event() && s_cur != PAGE_HOME) {
        go_to(PAGE_HOME, true);
        return;
    }
    if (b == BTN_SHORT || g.type == G_SWIPE_L) {
        go_to((s_cur + 1) % N_PAGES, true);
        return;
    }
    if (g.type == G_SWIPE_R) {
        go_to((s_cur + N_PAGES - 1) % N_PAGES, true);
        return;
    }
    if (g.type == G_TAP && s_pages[s_cur].touch) {
        s_pages[s_cur].touch(g.x, g.y);
    }

    if (s_pages[s_cur].step) s_pages[s_cur].step();

    int saver_min = settings_get()->saver_min;
    if (saver_min > 0 && now - s_last_input_us > (int64_t)saver_min * 60 * 1000000) {
        if (s_pages[s_cur].leave) s_pages[s_cur].leave();
        s_saver = true;
        saver_enter();
    }
}


void pages_redraw(void)
{
    s_last_input_us = esp_timer_get_time();  // el tiempo en la pantalla modal no cuenta como inactividad
    enter(s_cur);
}
