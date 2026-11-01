#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WEATHER_HOURS 12

typedef struct {
    uint32_t seq;                 // 0 = sin datos; cambia en cada actualizacion valida
    float temp, hum, wind;
    int code;
    bool is_day;
    float t_min, t_max;
    char sunrise[6], sunset[6];   // "HH:MM"
    float hourly[WEATHER_HOURS];
    int first_hour;               // hora local del primer valor de hourly
} weather_t;

void weather_update(void); // llamar solo desde la tarea net (único escritor de la ventana)
bool weather_get(weather_t *out);  // copia thread-safe; false si todavia no hay datos
const char *weather_text(int code);
