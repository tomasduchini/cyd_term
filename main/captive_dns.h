#pragma once
#include <stdint.h>

void captive_dns_start(uint32_t ip);  // ip en orden de red: responde todo con esa direccion
void captive_dns_stop(void);
