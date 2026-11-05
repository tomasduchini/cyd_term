#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"
#include "console.h"
#include "net.h"
#include "esp_log.h"
#include "weather.h"
#include "netmon.h"
#include "provision.h"
#include "settings.h"
#include "esp_timer.h"

#define BIT_CONNECTED BIT0
#define CONNECT_TIMEOUT_MS 30000  // sin conexion en este tiempo: modo configuracion
#define PROV_RETRY_MS      60000  // en modo configuracion, cada cuanto reintentar la red guardada

static volatile bool s_prov;      // modo configuracion activo: no reconectar solo
static bool s_have_creds, s_force;
static char s_ssid[PROV_SSID_MAX], s_pass[PROV_PASS_MAX];

static EventGroupHandle_t s_ev;
static char s_ip[16];

static volatile bool s_weather_now;

void net_request_weather(void)
{
    s_weather_now = true;
}

// Corre en la tarea del event loop: solo levanta/baja bits, no escribe en la ventana
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds && !s_prov) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW("net", "desconectado, reason=%d", d->reason);
        if (!s_prov) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

static void net_task(void *arg)
{
    char line[48];

    bool connected = false;
    if (!s_prov) {
        snprintf(line, sizeof line, CON_C_DIM "wifi: conectando a %.24s\n", s_ssid);
        con_type(line);
        connected = xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) & BIT_CONNECTED;
        if (!connected) con_type(CON_C_WARN "wifi: no conecta\n");
    } else if (!s_have_creds) {
        con_type(CON_C_WARN "wifi: sin red configurada\n");
    }

    if (!connected) {
        s_prov = true;
        prov_start();
        // con red guardada (y sin pedido manual) se reintenta cada tanto:
        // cubre el caso del router que vuelve mas tarde despues de un corte de luz
        bool retry = s_have_creds && !s_force;
        while (!(xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE,
                                     pdMS_TO_TICKS(PROV_RETRY_MS)) & BIT_CONNECTED)) {
            if (retry) esp_wifi_connect();
        }
        prov_stop();
        s_prov = false;
    }
    snprintf(line, sizeof line, CON_C_TEXT "wifi: ok " CON_C_HI "%s\n", s_ip);
    con_type(line);
    netmon_start();

    con_type(CON_C_DIM "ntp: sincronizando...\n");
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        con_type(CON_C_WARN "ntp: timeout, sigo esperando\n");
    }
    con_type(CON_C_TEXT "ntp: hora sincronizada\n" CON_PAUSE_1S CON_PAUSE_1S CON_PAUSE_1S CON_CLEAR CON_EVENT);

    int retry_s = 10;
    while (1) {
        if (weather_update()) {
            retry_s = 10;
            // espera en pasos de 1 s: si se cambia el intervalo en config, se aplica enseguida
            int64_t t0 = esp_timer_get_time();
            while (!s_weather_now &&
                   esp_timer_get_time() - t0 < (int64_t)settings_get()->weather_min * 60 * 1000000) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            s_weather_now = false;
        } else {
            snprintf(line, sizeof line, CON_C_DIM "clima: reintento en %ds\n", retry_s);
            con_type(line);
            vTaskDelay(pdMS_TO_TICKS(retry_s * 1000));
            retry_s *= 2;
            int period_s = settings_get()->weather_min * 60;
            if (retry_s > period_s) retry_s = period_s;
        }
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    s_ev = xEventGroupCreate();
    assert(s_ev);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t sta = { 0 };
    s_have_creds = prov_load(s_ssid, s_pass);
    s_force = prov_take_force();
    s_prov = s_force || !s_have_creds;  // antes de esp_wifi_start: STA_START no debe conectar
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, s_pass, sizeof sta.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    setenv("TZ", CONFIG_TERM_TZ, 1);
    tzset();

    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 2, NULL, 1);  // nucleo 1: no le saca CPU al render
    ESP_ERROR_CHECK(esp_wifi_start());
}
