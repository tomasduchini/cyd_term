#include <time.h>
#include "sdkconfig.h"
#include "term.h"
#include "rain.h"
#include "frame.h"
#include "backlight.h"
#include "bigclock.h"
#include "saver.h"
#include "settings.h"

#define COL_DIGIT 0xCFDF

// Recuadro central sin marco: reloj + 2 celdas de aire a los lados y 1 arriba/abajo
#define BOX_W  (BIGCLOCK_W + 4)
#define BOX_H  (BIGCLOCK_H + 2)
#define BOX_C0 ((TERM_COLS - BOX_W) / 2)
#define BOX_R0 ((TERM_ROWS - BOX_H) / 2)

static time_t s_last;

static void draw(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    bool synced = tm.tm_year + 1900 >= 2025;
    bigclock_draw(BOX_C0 + 2, BOX_R0 + 1, &tm, synced, !synced || tm.tm_sec % 2 == 0, COL_DIGIT);
}

void saver_enter(void)
{
    frame_hide();
    rain_init();  // limpia la pantalla entera
    rain_set_mask(BOX_C0, BOX_R0, BOX_C0 + BOX_W - 1, BOX_R0 + BOX_H - 1);
    backlight_set(settings_get()->saver_bright);
    s_last = time(NULL);
    draw(s_last);
}

void saver_step(void)
{
    rain_step();
    time_t now = time(NULL);
    if (now != s_last) {
        s_last = now;
        draw(now);
    }
}

void saver_exit(void)
{
    backlight_set(settings_get()->brightness);
}
