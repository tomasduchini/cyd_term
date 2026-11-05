#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PC_HIST     16   // historial de carga de CPU/GPU (1 muestra por segundo)
#define PC_FPS_HIST 20
#define PC_NODATA   (-1.0f)

typedef struct {
    uint32_t seq;   // 0 = nunca llego nada
    int64_t t_us;   // ultima linea recibida (esp_timer)
    float cpu, cput, gpu, gput, ram, ramu, ramt, fps;  // PC_NODATA si no vino
    float cpu_h[PC_HIST], gpu_h[PC_HIST], fps_h[PC_FPS_HIST];  // de la mas vieja a la mas nueva
    float fps1;                 // 1 % low; PC_NODATA si no vino
    float fps1_h[PC_FPS_HIST];
    char app[24];   // proceso de la ventana activa; "" si no vino
} pc_t;

void pcstat_start(void);
bool pcstat_get(pc_t *out);  // false si todavia no llego nada
