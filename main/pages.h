#pragma once
#include <stdbool.h>

void pages_init(void); // configura el boton y entra a la primera página
void pages_step(void); // boton BOOT + step de la pagina activa
bool pages_busy(void);  // true durante una transicion
void pages_redraw(void);  // redibuja la pagina actual completa (despues de una pantalla modal)
