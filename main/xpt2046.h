#pragma once
#include <stdbool.h>

typedef struct {
	int x, y;             // pixeles de pantalla (0..319, 0..239)
	int raw_x, raw_y, z;  // crudos del XPT2046, para calibrar
}xpt2046_point_t;

void xpt2046_init(void);
bool xpt2046_read(xpt2046_point_t *p);
