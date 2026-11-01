#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "console.h"
#include "weather.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define URL_FMT "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s" \
                "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code,is_day" \
                "&hourly=temperature_2m&forecast_hours=12" \
                "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset&forecast_days=1" \
                "&timezone=auto"

typedef struct {
    char *buf;
    int len;
    int cap;
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        resp_t *r = e->user_data;
        int n = e->data_len;
        if (r->len + n > r->cap - 1) n = r->cap - 1 - r->len;
        if (n > 0) {
            memcpy(r->buf + r->len, e->data, n);
            r->len += n;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool json_num(const char *obj, const char *key, float *out)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p) return false;
    p += strlen(pat);
    char *end;
    *out = strtof(p, &end);
    return end != p;
}

static int json_arr(const char *obj, const char *key, float *out, int max)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char *p = strstr(obj, pat);
    if (!p) return 0;
    p += strlen(pat);
    int n = 0;
    while (n < max && *p && *p != ']') {
        char *end;
        out[n] = strtof(p, &end);
        if (end == p) break;  // null u otro valor no numerico
        n++;
        p = end;
        if (*p == ',') p++;
    }
    return n;
}

static bool json_hhmm(const char *obj, const char *key, char out[6])  // primer elemento "....THH:MM"
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":[\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), 'T');
    if (!p || strlen(p) < 6) return false;
    memcpy(out, p + 1, 5);
    out[5] = '\0';
    return true;
}

static weather_t s_data;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

const char *weather_text(int c)
{
    if (c == 0)               return "despejado";
    if (c <= 2)               return "poco nublado";
    if (c == 3)               return "nublado";
    if (c == 45 || c == 48)   return "niebla";
    if (c >= 51 && c <= 57)   return "llovizna";
    if (c >= 61 && c <= 67)   return "lluvia";
    if (c >= 71 && c <= 77)   return "nieve";
    if (c >= 80 && c <= 82)   return "chaparrones";
    if (c >= 85 && c <= 86)   return "nevadas";
    if (c >= 95)              return "tormenta";
    return "?";
}

void weather_update(void)
{
    static char body[2048];
    char url[384];
    char line[64];
    resp_t resp = { .buf = body, .len = 0, .cap = sizeof body };
    body[0] = '\0';

    snprintf(url, sizeof url, URL_FMT, CONFIG_TERM_LAT, CONFIG_TERM_LON);

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) {
        snprintf(line, sizeof line, CON_C_WARN "clima: %s (%d)\n", esp_err_to_name(err), status);
        con_type(line);
        return;
    }

    const char *cur    = strstr(body, "\"current\":{");
    const char *hourly = strstr(body, "\"hourly\":{");
    const char *daily  = strstr(body, "\"daily\":{");
    weather_t w = { 0 };
    float code, isday;
    char hh[6];
    if (!cur || !hourly || !daily ||
        !json_num(cur, "temperature_2m", &w.temp) ||
        !json_num(cur, "relative_humidity_2m", &w.hum) ||
        !json_num(cur, "wind_speed_10m", &w.wind) ||
        !json_num(cur, "weather_code", &code) ||
		!json_num(cur, "is_day", &isday) ||
        json_arr(hourly, "temperature_2m", w.hourly, WEATHER_HOURS) != WEATHER_HOURS ||
        !json_hhmm(hourly, "time", hh) ||
        json_arr(daily, "temperature_2m_min", &w.t_min, 1) != 1 ||
        json_arr(daily, "temperature_2m_max", &w.t_max, 1) != 1 ||
        !json_hhmm(daily, "sunrise", w.sunrise) ||
        !json_hhmm(daily, "sunset", w.sunset)) {
        ESP_LOGW("weather", "parseo fallido, %d bytes:\n%s", resp.len, body);
        con_type(CON_C_WARN "clima: respuesta invalida\n");
        return;
    }
    w.code = (int)code;
    w.is_day = isday > 0.5f;
    w.first_hour = atoi(hh);

    taskENTER_CRITICAL(&s_mux);
    w.seq = s_data.seq + 1;
    s_data = w;
    taskEXIT_CRITICAL(&s_mux);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(line, sizeof line, CON_C_DIM "[%02d:%02d] " CON_C_TEXT "clima: %.1fC %s\n",
             tm.tm_hour, tm.tm_min, w.temp, weather_text(w.code));
    con_type(line);
}

bool weather_get(weather_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_data;
    taskEXIT_CRITICAL(&s_mux);
    return out->seq != 0;
}
