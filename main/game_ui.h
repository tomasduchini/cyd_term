#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "frame.h"

// Recuadro entandar de "como jugar"
#define GUI_BOX_C0 (FRAME_C0 + 4)
#define GUI_BOX_C1 (FRAME_C0 + 35)
#define GUI_BOX_R0 (FRAME_R0 + 2)
#define GUI_BOX_R1 (FRAME_R0 + 12)

void gui_box(int c0, int r0, int c1, int r1, uint16_t fg);  //borde + interior limpio
void gui_clear(int c0, int r0, int c1, int r1);
void gui_center(int c, int r, int w, const char *s, uint16_t fg);  // texto centrado en w celdas
void gui_help(const char *const keys[], const char *const vals[], int n);  // hasta 6 filas
