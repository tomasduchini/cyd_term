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
    char cpu_name[24], gpu_name[24], ram_name[24];   // modelos ("" si no llegaron)
} pc_t;

void pcstat_start(void);
bool pcstat_get(pc_t *out);  // false si todavia no llego nada
typedef struct {
    uint32_t seq;
    int64_t t_us;                       // cuando llego (esp_timer)
    int st;                             // 0 nada, 1 reproduciendo, 2 pausado
    int pos, dur;                       // segundos; dur = 0 si la app no informa
    char title[48], artist[48], app[16];
} pc_media_t;

bool pcstat_get_media(pc_media_t *out);  // false si nunca llego
void pcstat_send_cmd(const char *cmd);   // "play", "next", "prev"
