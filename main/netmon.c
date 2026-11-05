#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "ping/ping_sock.h"
#include "netmon.h"
#include <errno.h>
#include "lwip/sockets.h"

#define PING_INTERVAL_MS 2000
#define PING_TIMEOUT_MS  1000
#define RSSI_PERIOD_US   (2 * 1000000)
#define PROBE_IP   "1.1.1.1"
#define PROBE_PORT 443

static netmon_t s_data;
static bool s_started;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void push(int16_t *arr, int16_t v)
{
    taskENTER_CRITICAL(&s_mux);
    memmove(arr, arr + 1, (NETMON_HIST - 1) * sizeof arr[0]);
    arr[NETMON_HIST - 1] = v;
    s_data.seq++;
    taskEXIT_CRITICAL(&s_mux);
}

static void on_ping_ok(esp_ping_handle_t h, void *arg)
{
    uint32_t ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof ms);
    push((int16_t *)arg, ms > 30000 ? 30000 : (int16_t)ms);
}

static void on_ping_timeout(esp_ping_handle_t h, void *arg)
{
    push((int16_t *)arg, -1);
}

static void start_ping(const ip_addr_t *target, int16_t *series)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = *target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = PING_TIMEOUT_MS;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_ok,
        .on_ping_timeout = on_ping_timeout,
        .cb_args = series,
    };
    esp_ping_handle_t h;
    if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) {
        esp_ping_start(h);
    } else {
        ESP_LOGW("netmon", "no se pudo crear la sesion de ping");
    }
}

static void on_rssi_timer(void *arg)
{
    wifi_ap_record_t ap;
    push(s_data.rssi, esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : NETMON_NODATA);
}

static int tcp_connect_ms(void)  // ms hasta completar el handshake, -1 si falla o timeout
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PROBE_PORT),
    };
    addr.sin_addr.s_addr = inet_addr(PROBE_IP);

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

    int ms = -1;
    int64_t t0 = esp_timer_get_time();
    int r = connect(s, (struct sockaddr *)&addr, sizeof addr);
    if (r == 0) {
        ms = (int)((esp_timer_get_time() - t0) / 1000);
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval tv = {
            .tv_sec = PING_TIMEOUT_MS / 1000,
            .tv_usec = (PING_TIMEOUT_MS % 1000) * 1000,
        };
        if (select(s + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) ms = (int)((esp_timer_get_time() - t0) / 1000);
        }
    }

    struct linger lg = { .l_onoff = 1, .l_linger = 0 };  // cierre con RST: sin TIME_WAIT
    setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(s);
    return ms;
}

static void tcp_probe_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    while (1) {
        int ms = tcp_connect_ms();
        push(s_data.inet, ms < 0 ? -1 : (ms > 30000 ? 30000 : ms));
        xTaskDelayUntil(&last, pdMS_TO_TICKS(PING_INTERVAL_MS));
    }
}
void netmon_start(void)
{
    if (s_started) return;

    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NETMON_HIST; i++) {
        s_data.gw[i] = s_data.inet[i] = s_data.rssi[i] = NETMON_NODATA;
    }
    taskEXIT_CRITICAL(&s_mux);

    esp_netif_ip_info_t info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        ip_addr_t gw;
        IP_ADDR4(&gw, 0, 0, 0, 0);
        ip_2_ip4(&gw)->addr = info.gw.addr;
        start_ping(&gw, s_data.gw);
    }

    xTaskCreatePinnedToCore(tcp_probe_task, "tcpprobe", 4096, NULL, 2, NULL, 1);

    const esp_timer_create_args_t targs = { .callback = on_rssi_timer, .name = "rssi" };
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, RSSI_PERIOD_US));

    s_started = true;
}

bool netmon_get(netmon_t *out)
{
    if (!s_started) return false;
    taskENTER_CRITICAL(&s_mux);
    *out = s_data;
    taskEXIT_CRITICAL(&s_mux);
    return true;
}
