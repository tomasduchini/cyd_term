#pragma once
#include <stdint.h>
#include <stdbool.h>

// Rectángulo del marco (inclusivo, en celdas). El interior es el area del contenido
#define FRAME_C0 0
#define FRAME_R0 0
#define FRAME_C1 39
#define FRAME_R1 14

void frame_draw(const char *title, uint16_t accent, bool show_status);  // borde + titulo, limpia el interior
void frame_status(const char *s); // texto en el borde inferior, a la derecha
void frame_dots(int cur, int n);  // indicadores de pagina en el borde inferior
void frame_hide(void);  // deja de dibujar el estado hasta el proximo frame_draw
