#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "term.h"
#include "frame.h"
#include "page_wifi.h"
#include "provision.h"

#define COL_BG    0x0000
#define COL_TEXT  0xF65F  // lila muy claro
#define COL_MAIN  0xE21F  // magenta
#define COL_DIM   0x8AD2  // lila apagado
#define COL_TRACK 0x38E8  // violeta oscuro

#define CH_H     0x88  // ─
#define CH_VBAR1 0xE0  // barra vertical angosta, niveles 1..8

#define MAX_AP        20
#define LIST_ROWS     7
#define N_CH          13
#define SCAN_EVERY_US (10 * 1000000LL)
#define RETRY_US      (2 * 1000000LL)
#define SCAN_MAX_US   (10 * 1000000LL)  // si no llega SCAN_DONE, se libera el estado
#define DETAIL_US (15 * 1000000LL)

// Filas
#define STAT_R (FRAME_R0 + 1)   // estado del escaneo
#define HDR_R  (FRAME_R0 + 2)   // encabezados
#define LIST_R (FRAME_R0 + 3)   // primera red
#define SEP_R  (FRAME_R0 + 10)  // separador
#define HIST_R (FRAME_R0 + 11)  // fila superior del histograma (2 celdas)
#define HLBL_R (FRAME_R0 + 13)  // numeros de canal

// Columnas
#define X_MARK (FRAME_C0 + 2)
#define X_SIG  (FRAME_C0 + 3)   // 4 celdas
#define X_DBM  (FRAME_C0 + 8)   // 4 caracteres
#define X_CH   (FRAME_C0 + 13)  // 3 caracteres
#define X_SEG  (FRAME_C0 + 17)  // 4 caracteres
#define X_SSID (FRAME_C0 + 22)
#define SSID_W 16
#define HIST_C (FRAME_C0 + 3)   // 13 barras, una cada 2 celdas
#define SUM_C  (FRAME_C0 + 30)  // resumen, 8 caracteres

static wifi_ap_record_t s_recs[MAX_AP];
static int s_n, s_total;
static volatile bool s_scan_done;
static bool s_scanning, s_have_data, s_handler;
static int64_t s_next_scan_us, s_scan_start_us;
static int s_spin;
static bool s_sel_active;      // ficha de una red abierta
static uint8_t s_sel_bssid[6];
static int64_t s_sel_until;

static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_scan_done = true;  // los registros se leen desde el loop principal
}

static void start_scan(void)
{
    wifi_scan_config_t cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 60, .max = 120 },
    };
    int64_t now = esp_timer_get_time();
    if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {
        s_scanning = true;
        s_scan_start_us = now;
    } else {
        s_next_scan_us = now + RETRY_US;  // p. ej. net reconectando
    }
}

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static void fetch_results(void)
{
    uint16_t total = 0, n = MAX_AP;
    esp_wifi_scan_get_ap_num(&total);
    if (esp_wifi_scan_get_ap_records(&n, s_recs) != ESP_OK) n = 0;  // libera la lista del driver
    s_total = total;
    s_n = n;
    qsort(s_recs, s_n, sizeof s_recs[0], by_rssi);

    // la red conectada va primera
    wifi_ap_record_t cur;
    if (esp_wifi_sta_get_ap_info(&cur) == ESP_OK) {
        for (int i = 1; i < s_n; i++) {
            if (!memcmp(s_recs[i].bssid, cur.bssid, 6)) {
                wifi_ap_record_t t = s_recs[i];
                memmove(&s_recs[1], &s_recs[0], i * sizeof s_recs[0]);
                s_recs[0] = t;
                break;
            }
        }
    }
    s_have_data = true;
}

static const char *auth_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ent";
    default:                        return "?";
    }
}

static const char *auth_long(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "abierta";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 Enterprise";
    default:                        return "otra";
    }
}

static const char *phy_str(const wifi_ap_record_t *ap)
{
    static char s[12];
    s[0] = '\0';
    if (ap->phy_11b)  strcat(s, "b/");
    if (ap->phy_11g)  strcat(s, "g/");
    if (ap->phy_11n)  strcat(s, "n/");
    if (ap->phy_11ax) strcat(s, "ax/");
    size_t n = strlen(s);
    if (n) s[n - 1] = '\0';  // saca la ultima barra
    return s;
}

static int sig_level(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

static void ssid_ascii(const uint8_t *src, char *dst, int max)
{
    int n = 0;
    for (const uint8_t *p = src; *p && n < max; p++) {
        if (*p >= 0x80 && *p < 0xC0) continue;  // continuacion UTF-8: se descarta
        dst[n++] = (*p >= 0x20 && *p <= 0x7E) ? (char)*p : '?';
    }
    dst[n] = '\0';
    if (n == 0) strcpy(dst, "<oculta>");
}

static void clear_row(int r)
{
    for (int c = X_MARK; c < X_SSID + SSID_W; c++) {
        term_put(c, r, ' ', COL_BG, COL_BG);
    }
}

static void draw_static(void);
static void draw_list(void);
static void detail_kv(int r, const char *label, const char *value)
{
    char line[40];
    snprintf(line, sizeof line, "%-7s", label);
    term_print(X_SIG, r, line, COL_DIM, COL_BG);
    snprintf(line, sizeof line, "%-27.27s", value);
    term_print(X_SIG + 7, r, line, COL_TEXT, COL_BG);
}

static void draw_detail(const wifi_ap_record_t *cur)  // cur: red conectada, o NULL
{
    char line[40], buf[40];
    snprintf(line, sizeof line, "%-35s", "detalle de la red");
    term_print(X_SIG, HDR_R, line, COL_DIM, COL_BG);
    for (int i = 0; i < LIST_ROWS; i++) clear_row(LIST_R + i);

    const wifi_ap_record_t *ap = NULL;
    for (int i = 0; i < s_n; i++) {
        if (!memcmp(s_recs[i].bssid, s_sel_bssid, 6)) {
            ap = &s_recs[i];
            break;
        }
    }
    if (!ap) {
        term_print(X_SIG, LIST_R + 2, "la red ya no aparece en el escaneo", COL_DIM, COL_BG);
        term_print(X_SIG, LIST_R + 6, "toca para volver", COL_DIM, COL_BG);
        return;
    }

    char ssid[33];
    ssid_ascii(ap->ssid, ssid, 32);
    term_print(X_SIG, LIST_R, ssid, COL_MAIN, COL_BG);

    const uint8_t *b = ap->bssid;
    snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    detail_kv(LIST_R + 1, "bssid", buf);

    snprintf(buf, sizeof buf, "%d  (802.11 %s)", ap->primary, phy_str(ap));
    detail_kv(LIST_R + 2, "canal", buf);

    detail_kv(LIST_R + 3, "seg", auth_long(ap->authmode));

    int q = 2 * (ap->rssi + 100);  // aproximacion habitual: -50 dBm = 100 %, -100 dBm = 0 %
    if (q < 0) q = 0;
    if (q > 100) q = 100;
    snprintf(buf, sizeof buf, "%d dBm  (%d%%)", ap->rssi, q);
    detail_kv(LIST_R + 4, "senal", buf);

    char saved[PROV_SSID_MAX], pass[PROV_PASS_MAX];
    bool conn = cur && !memcmp(cur->bssid, ap->bssid, 6);
    bool known = prov_load(saved, pass) && !strcmp(saved, (const char *)ap->ssid);
    detail_kv(LIST_R + 5, "estado", conn ? "conectada" : known ? "guardada" : "-");

    term_print(X_SIG, LIST_R + 6, "toca para volver", COL_DIM, COL_BG);
}

static void close_detail(void)
{
    s_sel_active = false;
    for (int c = X_MARK; c < X_SSID + SSID_W; c++) term_put(c, HDR_R, ' ', COL_BG, COL_BG);
    draw_static();
    draw_list();
}

static void draw_row(int r, const wifi_ap_record_t *ap, bool mine)
{
    char buf[40], ssid[SSID_W + 1];
    uint16_t fg = mine ? COL_MAIN : COL_TEXT;

    term_put(X_MARK, r, mine ? '>' : ' ', mine ? COL_MAIN : COL_BG, COL_BG);

    int lv = sig_level(ap->rssi);
    for (int i = 0; i < 4; i++) {  // barras de 2, 4, 6 y 8 niveles
        term_put(X_SIG + i, r, CH_VBAR1 + 2 * (i + 1) - 1, i < lv ? COL_MAIN : COL_TRACK, COL_BG);
    }

    snprintf(buf, sizeof buf, "%4d", ap->rssi);
    term_print(X_DBM, r, buf, fg, COL_BG);
    snprintf(buf, sizeof buf, "%3d", ap->primary);
    term_print(X_CH, r, buf, fg, COL_BG);
    snprintf(buf, sizeof buf, "%-4s", auth_str(ap->authmode));
    term_print(X_SEG, r, buf, mine ? COL_MAIN : COL_DIM, COL_BG);

    ssid_ascii(ap->ssid, ssid, SSID_W);
    snprintf(buf, sizeof buf, "%-*s", SSID_W, ssid);
    term_print(X_SSID, r, buf, fg, COL_BG);
}

static void draw_static(void)
{
    term_print(X_SIG,  HDR_R, "sen",  COL_DIM, COL_BG);
    term_print(X_DBM,  HDR_R, " dBm", COL_DIM, COL_BG);
    term_print(X_CH,   HDR_R, " ch",  COL_DIM, COL_BG);
    term_print(X_SEG,  HDR_R, "seg",  COL_DIM, COL_BG);
    term_print(X_SSID, HDR_R, "ssid", COL_DIM, COL_BG);

    for (int c = FRAME_C0 + 1; c < FRAME_C1; c++) {
        bool on = (c - FRAME_C0) % 2;
        term_put(c, SEP_R, on ? CH_H : ' ', on ? COL_TRACK : COL_BG, COL_BG);
    }
    term_print(FRAME_C0 + 3, SEP_R, " canales 2.4 GHz ", COL_DIM, COL_BG);

    term_print(HIST_C,      HLBL_R, "1",  COL_DIM, COL_BG);
    term_print(HIST_C + 10, HLBL_R, "6",  COL_DIM, COL_BG);
    term_print(HIST_C + 20, HLBL_R, "11", COL_DIM, COL_BG);
    term_print(HIST_C + 24, HLBL_R, "13", COL_DIM, COL_BG);
}

static void draw_summary(int r, const char *label, int value)
{
    char buf[8];
    term_print(SUM_C, r, label, COL_DIM, COL_BG);
    if (value > 0) {
        snprintf(buf, sizeof buf, "%2d", value > 99 ? 99 : value);
    } else {
        snprintf(buf, sizeof buf, "--");
    }
    term_print(SUM_C + 6, r, buf, COL_TEXT, COL_BG);
}

static void draw_list(void)
{
    wifi_ap_record_t cur;
    bool connected = esp_wifi_sta_get_ap_info(&cur) == ESP_OK;

    if (s_sel_active) {
        draw_detail(connected ? &cur : NULL);  // se actualiza con cada escaneo
    } else {
        for (int i = 0; i < LIST_ROWS; i++) {
            if (i < s_n) {
                draw_row(LIST_R + i, &s_recs[i], connected && !memcmp(s_recs[i].bssid, cur.bssid, 6));
            } else {
                clear_row(LIST_R + i);
            }
        }
    }

    // histograma de ocupacion por canal
    int counts[N_CH] = { 0 }, maxc = 1;
    for (int i = 0; i < s_n; i++) {
        int ch = s_recs[i].primary;
        if (ch >= 1 && ch <= N_CH) counts[ch - 1]++;
    }
    for (int c = 0; c < N_CH; c++) {
        if (counts[c] > maxc) maxc = counts[c];
    }
    for (int c = 0; c < N_CH; c++) {
        int level = (counts[c] * 16 + maxc - 1) / maxc;  // 0..16, >= 1 si hay alguna red
        int x = HIST_C + 2 * c;
        uint16_t fg = (connected && cur.primary == c + 1) ? COL_TEXT : COL_MAIN;
        if (level == 0) {
            term_put(x, HIST_R + 1, CH_VBAR1, COL_TRACK, COL_BG);  // canal vacio: base tenue
        } else {
            term_put(x, HIST_R + 1, CH_VBAR1 + (level > 8 ? 8 : level) - 1, fg, COL_BG);
        }
        term_put(x, HIST_R, level > 8 ? CH_VBAR1 + level - 9 : ' ', level > 8 ? fg : COL_BG, COL_BG);
    }

    // mejor canal entre 1, 6 y 11: menos redes en los +-4 canales que se solapan
    static const int cand[3] = { 1, 6, 11 };
    int best = 0, best_load = 1 << 30;
    for (int k = 0; k < 3; k++) {
        int load = 0;
        for (int c = 0; c < N_CH; c++) {
            if (abs((c + 1) - cand[k]) <= 4) load += counts[c];
        }
        if (load < best_load) {
            best_load = load;
            best = cand[k];
        }
    }

    draw_summary(HIST_R,     "redes", s_total);
    draw_summary(HIST_R + 1, "mejor", best);
    draw_summary(HLBL_R,     "tu ch", connected ? cur.primary : 0);
}

static void draw_status(int64_t now)
{
    char buf[SSID_W + 1];
    if (s_scanning) {
        static const char spin[] = "|/-\\";
        snprintf(buf, sizeof buf, "escaneando %c", spin[(s_spin++ / 4) % 4]);
    } else if (s_have_data) {
        int s = (int)((s_next_scan_us - now + 999999) / 1000000);
        if (s < 0) s = 0;
        if (s > 99) s = 99;
        snprintf(buf, sizeof buf, "prox scan %2ds", s);
    } else {
        snprintf(buf, sizeof buf, "esperando wifi");
    }
    char line[SSID_W + 1];
    snprintf(line, sizeof line, "%*s", SSID_W, buf);  // alineado a la derecha
    term_print(X_SSID, STAT_R, line, COL_DIM, COL_BG);
}

void wifi_enter(void)
{
    s_sel_active = false;
    if (!s_handler) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_scan_done, NULL));
        s_handler = true;
    }
    draw_static();
    if (s_have_data) draw_list();
    if (!s_scanning) start_scan();  // datos frescos al entrar
}

void wifi_step(void)
{
    int64_t now = esp_timer_get_time();

    if (s_scan_done) {
        s_scan_done = false;
        s_scanning = false;
        fetch_results();
        draw_list();
        s_next_scan_us = now + SCAN_EVERY_US;
    } else if (s_scanning && now - s_scan_start_us > SCAN_MAX_US) {
        s_scanning = false;  // nunca llego SCAN_DONE
        s_next_scan_us = now + RETRY_US;
    } else if (!s_scanning && now >= s_next_scan_us) {
        start_scan();
    }

    if (s_sel_active && now > s_sel_until) close_detail();
    draw_status(now);
}


void wifi_touch(int x, int y)
{
    int row = y / 16;
    if (row < HDR_R || row >= LIST_R + LIST_ROWS) return;  // solo la zona de la lista
    if (s_sel_active) {
        close_detail();
        return;
    }
    int idx = row - LIST_R;
    if (idx < 0 || idx >= s_n) return;
    memcpy(s_sel_bssid, s_recs[idx].bssid, 6);
    s_sel_active = true;
    s_sel_until = esp_timer_get_time() + DETAIL_US;
    draw_list();
}
