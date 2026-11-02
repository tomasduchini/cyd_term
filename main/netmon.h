#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NETMON_N      24          // muestras por serie (una cada 2 s)
#define NETMON_NODATA INT16_MIN

typedef struct {
    uint32_t seq;
    int16_t gw[NETMON_N];    // ms al router; -1 = sin respuesta
    int16_t inet[NETMON_N];  // ms a 1.1.1.1
    int16_t rssi[NETMON_N];  // dBm
} netmon_t;                  // series de la mas vieja a la mas nueva

void netmon_start(void);         // desde la tarea net, con IP asignada
bool netmon_get(netmon_t *out);  // false si todavia no arranco
