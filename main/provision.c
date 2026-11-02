#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "console.h"
#include "provision.h"
#include <stdlib.h>
#include "captive_dns.h"

#define NVS_NS "wifi"

static const char *TAG = "prov";
static httpd_handle_t s_http;
static esp_netif_t *s_ap_netif;

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>CYD-TERM</title><style>"
    "body{background:#000;color:#3ef0a0;font-family:monospace;padding:20px;max-width:420px;margin:auto}"
    "h1{font-size:20px;color:#cfffe0}"
    "input,button{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 16px;font:16px monospace;"
    "background:#0a1a10;color:#cfffe0;border:1px solid #3ef0a0}"
    "button{background:#3ef0a0;color:#000;font-weight:bold}"
    ".net{background:#0a1a10;color:#cfffe0;font-weight:normal;text-align:left;margin:0 0 6px}"
    "#nets{margin-bottom:10px}"
    "</style></head><body><h1>// cyd-term wifi</h1>"
    "<div id=nets>buscando redes...</div>"
    "<button type=button class=net onclick=scan(1)>&#8635; actualizar</button>"
    "<form method=post action=/save>"
    "<label>red (ssid)</label><input name=ssid id=ssid maxlength=32 required>"
    "<label>clave</label><input name=pass id=pass type=password maxlength=63>"
    "<button>guardar y reiniciar</button></form>"
    "<p>solo redes de 2,4 GHz</p>"
    "<script>"
    "function bars(r){return r>-55?'\\u2582\\u2584\\u2586\\u2588':r>-67?'\\u2582\\u2584\\u2586_':r>-75?'\\u2582\\u2584__':'\\u2582___'}"
    "async function scan(f){const el=document.getElementById('nets');if(f)el.textContent='buscando...';"
    "try{const r=await fetch('/scan'+(f?'?refresh=1':''));const l=await r.json();el.innerHTML='';"
    "l.forEach(n=>{const b=document.createElement('button');b.type='button';b.className='net';"
    "b.textContent=bars(n.rssi)+' '+n.ssid+(n.open?'  (abierta)':'');"
    "b.onclick=()=>{document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus()};"
    "el.appendChild(b)});if(!l.length)el.textContent='no se encontraron redes';"
    "}catch(e){el.textContent='error al buscar redes'}}"
    "scan(0);"
    "</script></body></html>";

static const char DONE_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'></head>"
    "<body style='background:#000;color:#3ef0a0;font-family:monospace;padding:20px'>"
    "<h1>guardado</h1><p>la placa se reinicia y se conecta a la red nueva.</p></body></html>";

// ---------------- NVS ----------------

bool prov_load(char *ssid, char *pass)
{
    nvs_handle_t h;
    ssid[0] = pass[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t ls = PROV_SSID_MAX, lp = PROV_PASS_MAX;
        if (nvs_get_str(h, "ssid", ssid, &ls) != ESP_OK) ssid[0] = '\0';
        if (nvs_get_str(h, "pass", pass, &lp) != ESP_OK) pass[0] = '\0';
        nvs_close(h);
    }
    if (!ssid[0]) {  // nada guardado: respaldo de menuconfig
        strlcpy(ssid, CONFIG_TERM_WIFI_SSID, PROV_SSID_MAX);
        strlcpy(pass, CONFIG_TERM_WIFI_PASSWORD, PROV_PASS_MAX);
    }
    return ssid[0] != '\0';
}

static esp_err_t save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool prov_take_force(void)
{
    nvs_handle_t h;
    uint8_t f = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, "force", &f) == ESP_OK && f) {
        nvs_erase_key(h, "force");
        nvs_commit(h);
    }
    nvs_close(h);
    return f != 0;
}

void prov_request_restart(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "force", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

// ---------------- Formulario ----------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    return (tolower((unsigned char)c) - 'a') + 10;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

// Busca key=valor en un cuerpo application/x-www-form-urlencoded y lo decodifica
static bool form_field(const char *body, const char *key, char *out, size_t max)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            char raw[200];
            size_t n = strcspn(p + kl + 1, "&");
            if (n >= sizeof raw) return false;
            memcpy(raw, p + kl + 1, n);
            raw[n] = '\0';
            url_decode(raw);
            if (strlen(raw) >= max) return false;
            strcpy(out, raw);
            return true;
        }
    }
    return false;
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "formulario invalido");
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    char ssid[PROV_SSID_MAX], pass[PROV_PASS_MAX];
    if (!form_field(body, "ssid", ssid, sizeof ssid) || !ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta el nombre de la red");
    }
    if (!form_field(body, "pass", pass, sizeof pass)) pass[0] = '\0';
    size_t pl = strlen(pass);
    if (pl > 0 && pl < 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "la clave tiene que tener entre 8 y 63 caracteres");
    }
    if (save_creds(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no se pudo guardar");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, DONE_PAGE);
    ESP_LOGI(TAG, "credenciales guardadas para '%s', reiniciando", ssid);

    // reinicio diferido: deja que la respuesta llegue al navegador
    const esp_timer_create_args_t a = { .callback = restart_cb, .name = "prov_rst" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 1500000);
    return ESP_OK;
}

// ---------------- Escaneo y portal cautivo ----------------

#define PROV_MAX_AP 20

static wifi_ap_record_t *s_aps;
static uint16_t s_nap;

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static void do_scan(void)  // bloqueante, ~2 s
{
    if (!s_aps) s_aps = calloc(PROV_MAX_AP, sizeof *s_aps);
    if (!s_aps) return;
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 80, .max = 150 },
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t n = PROV_MAX_AP;
    if (esp_wifi_scan_get_ap_records(&n, s_aps) == ESP_OK) {
        s_nap = n;
        qsort(s_aps, s_nap, sizeof s_aps[0], by_rssi);
    }
}

static void json_escape(const char *in, char *out, size_t max)
{
    size_t o = 0;
    for (; *in && o + 2 < max; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20) continue;  // control: se descarta
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = (char)c;      // UTF-8 pasa tal cual
    }
    out[o] = '\0';
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char q[24];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "refresh")) do_scan();

    const size_t cap = 2048;
    char *out = malloc(cap);
    if (!out) return httpd_resp_send_500(req);
    size_t o = 0;
    out[o++] = '[';
    bool first = true;
    for (int i = 0; i < s_nap; i++) {
        const char *ssid = (const char *)s_aps[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;  // las redes mesh repiten el SSID: queda la de mejor senal
        for (int j = 0; j < i && !dup; j++) dup = strcmp(ssid, (const char *)s_aps[j].ssid) == 0;
        if (dup) continue;

        char esc[72];
        json_escape(ssid, esc, sizeof esc);
        int w = snprintf(out + o, cap - o - 2, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                         first ? "" : ",", esc, s_aps[i].rssi,
                         s_aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        if (w < 0 || (size_t)w >= cap - o - 2) break;  // no entra: se corta la lista
        o += w;
        first = false;
    }
    out[o++] = ']';
    out[o] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, o);
    free(out);
    return err;
}

// Cualquier otra URL (las pruebas de conectividad de Android, iOS y Windows) va al portal
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------- Red propia + servidor ----------------

void prov_start(void)
{
    uint8_t mac[6];
    char ap_ssid[20], ap_pass[12], line[48];

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof ap_ssid, "CYD-TERM-%02X%02X", mac[4], mac[5]);
    snprintf(ap_pass, sizeof ap_pass, "%08lu", (unsigned long)(esp_random() % 100000000UL));

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    // El DHCP del AP anuncia a la propia placa como DNS (necesario para el portal cautivo)
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    esp_netif_dns_info_t dns = { .ip.u_addr.ip4.addr = ip.ip.addr, .ip.type = ESP_IPADDR_TYPE_V4 };
    uint8_t offer_dns = 0x02;  // DHCPS_OFFER_DNS
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                  ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof offer_dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    esp_netif_dhcps_start(s_ap_netif);  // si el AP todavia no arranco, lo inicia el propio driver

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, ap_pass, sizeof ap.ap.password);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;

    esp_wifi_disconnect();  // puede fallar si no estaba conectado: no importa
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    con_type(CON_C_WARN "modo configuracion wifi\n" CON_C_DIM "buscando redes...\n");
    do_scan();  // la lista queda lista para cuando se abra la pagina

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;  // los celulares abren muchas conexiones de prueba
    cfg.stack_size = 6144;        // el escaneo bloqueante corre en la tarea del servidor
    if (httpd_start(&s_http, &cfg) == ESP_OK) {
        const httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
        const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
        const httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET,  .handler = h_scan };
        const httpd_uri_t any  = { .uri = "/*",    .method = HTTP_GET,  .handler = h_redirect };
        httpd_register_uri_handler(s_http, &root);  // el orden importa: el comodin va al final
        httpd_register_uri_handler(s_http, &save);
        httpd_register_uri_handler(s_http, &scan);
        httpd_register_uri_handler(s_http, &any);
    } else {
        ESP_LOGE(TAG, "no se pudo iniciar el servidor web");
    }
    captive_dns_start(ip.ip.addr);

    snprintf(line, sizeof line, CON_C_TEXT "red:   " CON_C_HI "%s\n", ap_ssid);
    con_type(line);
    snprintf(line, sizeof line, CON_C_TEXT "clave: " CON_C_HI "%s\n", ap_pass);
    con_type(line);
    con_type(CON_C_DIM "la pagina se abre sola al conectarse\n"
             CON_C_TEXT "si no: " CON_C_HI "http://192.168.4.1\n" CON_C_TEXT);
}

void prov_stop(void)
{
    captive_dns_stop();
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
}
