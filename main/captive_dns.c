#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "captive_dns.h"

static const char *TAG = "dns";
static volatile bool s_run;
static int s_sock = -1;
static uint32_t s_ip;

static void dns_task(void *arg)
{
    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;  // incluye el timeout de 1 s (n = -1)

        uint16_t qd = (buf[4] << 8) | buf[5];
        if ((buf[2] & 0x80) || qd != 1) continue;  // solo consultas con una pregunta

        int p = 12;  // recorrer el nombre de la pregunta
        while (p < n && buf[p] != 0) p += buf[p] + 1;
        if (p + 5 > n) continue;
        p++;
        uint16_t qtype = (buf[p] << 8) | buf[p + 1];
        p += 4;  // tipo + clase
        int len = p;  // encabezado + pregunta

        buf[2] = 0x84 | (buf[2] & 0x01);  // respuesta, autoritativa, copia RD
        buf[3] = 0x80;                     // RA, sin error
        memset(buf + 6, 0, 6);             // sin respuestas por ahora

        if (qtype == 1 && len + 16 <= (int)sizeof buf) {  // tipo A: nuestra IP
            buf[7] = 1;
            uint8_t *a = buf + len;
            a[0] = 0xC0; a[1] = 0x0C;            // puntero al nombre de la pregunta
            a[2] = 0;    a[3] = 1;               // tipo A
            a[4] = 0;    a[5] = 1;               // clase IN
            a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60;  // TTL 60 s
            a[10] = 0;   a[11] = 4;
            memcpy(a + 12, &s_ip, 4);
            len += 16;
        }
        sendto(s_sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    close(s_sock);
    s_sock = -1;
    vTaskDelete(NULL);
}

void captive_dns_start(uint32_t ip)
{
    if (s_run) return;
    s_ip = ip;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "no se pudo crear el socket");
        return;
    }
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "no se pudo abrir el puerto 53");
        close(s_sock);
        s_sock = -1;
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };  // para poder salir del loop
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    s_run = true;
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 3, NULL, 1);
}

void captive_dns_stop(void)
{
    s_run = false;  // la tarea cierra el socket en menos de 1 s
}
