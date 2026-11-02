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
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "console.h"
#include "net.h"
#include "esp_log.h"
#include "weather.h"
#include "netmon.h"

#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_ev;
static char s_ip[16];

// Corre en la tarea del event loop: solo levanta/baja bits, no escribe en la ventana
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW("net", "desconectado, reason=%d", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

static void net_task(void *arg)
{
    char line[48];

    con_type(CON_C_DIM "wifi: conectando a " CONFIG_TERM_WIFI_SSID "\n");
    while (!(xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE,
                                 pdMS_TO_TICKS(10000)) & BIT_CONNECTED)) {
        con_type(CON_C_WARN "wifi: sin conexion, reintento\n");
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

    const int period_s = CONFIG_TERM_WEATHER_PERIOD_MIN * 60;
    int retry_s = 10;
    while (1) {
        if (weather_update()) {
            retry_s = 10;
            vTaskDelay(pdMS_TO_TICKS(period_s * 1000));
        } else {
            snprintf(line, sizeof line, CON_C_DIM "clima: reintento en %ds\n", retry_s);
            con_type(line);
            vTaskDelay(pdMS_TO_TICKS(retry_s * 1000));
            retry_s *= 2;
            if (retry_s > period_s) retry_s = period_s;
        }
    }
}

void net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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
    strlcpy((char *)sta.sta.ssid, CONFIG_TERM_WIFI_SSID, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, CONFIG_TERM_WIFI_PASSWORD, sizeof sta.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    setenv("TZ", CONFIG_TERM_TZ, 1);
    tzset();

    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 2, NULL, 1);  // nucleo 1: no le saca CPU al render
    ESP_ERROR_CHECK(esp_wifi_start());
}
