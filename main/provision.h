#pragma once
#include <stdbool.h>

#define PROV_SSID_MAX 33
#define PROV_PASS_MAX 65

bool prov_load(char *ssid, char *pass);  // NVS o, si no hay, menuconfig; false si no hay SSID
bool prov_take_force(void);              // true si se pidio modo configuracion (y limpia el pedido)
void prov_request_restart(void);         // marca el pedido y reinicia
void prov_start(void);                   // red propia + servidor web; escribe instrucciones (tarea net)
void prov_stop(void);                    // apaga el servidor y vuelve a modo STA
