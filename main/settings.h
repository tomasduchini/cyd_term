#pragma once
#include <stdint.h>

typedef struct {
    uint8_t brightness;    // % de brillo normal
    uint8_t saver_min;     // minutos sin uso hasta el salvapantallas; 0 = desactivado
    uint8_t saver_bright;  // % de brillo en el salvapantallas
    uint8_t weather_min;   // intervalo de actualizacion del clima
} settings_t;

void settings_init(void);                 // NVS + carga; llamar al arrancar, antes de usar
const settings_t *settings_get(void);
void settings_set(const settings_t *s);   // aplica ya y programa el guardado (loop principal)
void settings_step(void);                 // guardado diferido (loop principal)
void settings_save_now(void);             // forzar el guardado (antes de reiniciar)
