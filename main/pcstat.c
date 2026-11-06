#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "pcstat.h"

#define PC_UART  UART_NUM_0  // el mismo USB del monitor
#define PC_LINE_MAX 160

static pc_t s_data;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void hist_push(float *h, int n, float v)
{
    memmove(h, h + 1, (n - 1) * sizeof h[0]);
    h[n - 1] = v;
}

static void parse_info(char *s)  // "cpu=...;gpu=...;ram=..."
{
    char cpu[24] = "", gpu[24] = "", ram[24] = "";
    char *save;
    for (char *tok = strtok_r(s, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *dst = !strcmp(tok, "cpu") ? cpu : !strcmp(tok, "gpu") ? gpu : !strcmp(tok, "ram") ? ram : NULL;
        if (!dst) continue;
        strlcpy(dst, eq + 1, 24);
        for (char *q = dst; *q; q++) {
            if (*q < 0x20 || *q > 0x7E) *q = '?';  // nada de glifos propios
        }
    }
    taskENTER_CRITICAL(&s_mux);
    if (cpu[0]) strlcpy(s_data.cpu_name, cpu, sizeof s_data.cpu_name);
    if (gpu[0]) strlcpy(s_data.gpu_name, gpu, sizeof s_data.gpu_name);
    if (ram[0]) strlcpy(s_data.ram_name, ram, sizeof s_data.ram_name);
    taskEXIT_CRITICAL(&s_mux);
}

static void parse_line(char *line)
{
    if (strncmp(line, "PCI ", 4) == 0) {  // modelos de hardware: no son metricas
        parse_info(line + 4);
        return;
    }

    if (strncmp(line, "PC ", 3) != 0) return;

    float cpu = PC_NODATA, cput = PC_NODATA, gpu = PC_NODATA, gput = PC_NODATA;
    float ram = PC_NODATA, ramu = PC_NODATA, ramt = PC_NODATA, fps = PC_NODATA;
    float fps1 = PC_NODATA;
    char app[24] = "";

    char *save;
    for (char *tok = strtok_r(line + 3, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        float v = strtof(eq + 1, NULL);
        if      (!strcmp(tok, "cpu"))  cpu = v;
        else if (!strcmp(tok, "cput")) cput = v;
        else if (!strcmp(tok, "gpu"))  gpu = v;
        else if (!strcmp(tok, "gput")) gput = v;
        else if (!strcmp(tok, "ram"))  ram = v;
        else if (!strcmp(tok, "ramu")) ramu = v;
        else if (!strcmp(tok, "ramt")) ramt = v;
        else if (!strcmp(tok, "fps"))  fps = v;
        else if (!strcmp(tok, "fps1")) fps1 = v;
        else if (!strcmp(tok, "app")) {
            strlcpy(app, eq + 1, sizeof app);
            for (char *q = app; *q; q++) {
                if (*q < 0x20 || *q > 0x7E) *q = '?';  // nada de glifos propios
            }
        }
    }

    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_data.cpu = cpu;   s_data.cput = cput;
    s_data.gpu = gpu;   s_data.gput = gput;
    s_data.ram = ram;   s_data.ramu = ramu;  s_data.ramt = ramt;
    s_data.fps = fps;
    strlcpy(s_data.app, app, sizeof s_data.app);
    hist_push(s_data.cpu_h, PC_HIST, cpu);
    hist_push(s_data.gpu_h, PC_HIST, gpu);
    hist_push(s_data.fps_h, PC_FPS_HIST, fps);
    s_data.fps1 = fps1;
    hist_push(s_data.fps1_h, PC_FPS_HIST, fps1);
    s_data.t_us = now;
    s_data.seq++;
    taskEXIT_CRITICAL(&s_mux);
}

static void rx_task(void *arg)
{
    static char line[PC_LINE_MAX];
    int len = 0;
    uint8_t buf[64];

    while (1) {
        int n = uart_read_bytes(PC_UART, buf, sizeof buf, pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    parse_line(line);
                    len = 0;
                }
            } else if (len < PC_LINE_MAX - 1) {
                line[len++] = c;
            } else {
                len = 0;  // linea demasiado larga: se descarta
            }
        }
    }
}

void pcstat_start(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_data.cpu = s_data.cput = s_data.gpu = s_data.gput = PC_NODATA;
    s_data.ram = s_data.ramu = s_data.ramt = s_data.fps = PC_NODATA;
    for (int i = 0; i < PC_HIST; i++) s_data.cpu_h[i] = s_data.gpu_h[i] = PC_NODATA;
    for (int i = 0; i < PC_FPS_HIST; i++) s_data.fps_h[i] = PC_NODATA;
    s_data.fps1 = PC_NODATA;
    for (int i = 0; i < PC_FPS_HIST; i++) s_data.fps1_h[i] = PC_NODATA;
    taskEXIT_CRITICAL(&s_mux);

    // Solo RX: la consola sigue usando el mismo UART para los logs
    ESP_ERROR_CHECK(uart_driver_install(PC_UART, 1024, 0, 0, NULL, 0));
    xTaskCreatePinnedToCore(rx_task, "pc_rx", 3072, NULL, 2, NULL, 1);
}

bool pcstat_get(pc_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_data;
    taskEXIT_CRITICAL(&s_mux);
    return out->seq != 0;
}
