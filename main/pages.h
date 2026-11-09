#pragma once
#include <stdbool.h>

void pages_init(void); // configura el boton y entra a la primera página
void pages_step(void); // boton BOOT + step de la pagina activa
bool pages_busy(void);  // true durante una transicion
void pages_redraw(void);  // redibuja la pagina actual completa (despues de una pantalla modal)

// Modo captura (juegos): la pagina recibe todos los eventos y no hay navegacion
#define PG_DOWN    1   // dedo apoyado (instantaneo)
#define PG_TAP     2
#define PG_SWIPE_L 3
#define PG_SWIPE_R 4
#define PG_BUTTON  5   // boton BOOT, toque corto

typedef void (*pages_capture_fn)(int type, int x, int y);
void pages_set_capture(pages_capture_fn fn);  // NULL = navegacion normal
bool pages_touch_state(int *x, int *y);        // true mientras hay un dedo apoyado
