#pragma once
#include <stdbool.h>

typedef struct {
	int x, y;             // pixeles de pantalla (0..319, 0..239)
	int raw_x, raw_y, z;  // crudos del XPT2046, para calibrar
}xpt2046_point_t;

typedef struct {
    float ax, bx, ay, by;   // pixel = a * crudo + b, por eje (crudo con el intercambio de ejes aplicado)
} xpt2046_cal_t;

void xpt2046_init(void);
bool xpt2046_read(xpt2046_point_t *p);
bool xpt2046_read_raw(int *rx, int *ry);        // crudo promediado, con el intercambio de ejes aplicado
void xpt2046_set_cal(const xpt2046_cal_t *c);
