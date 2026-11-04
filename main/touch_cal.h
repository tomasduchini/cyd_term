#pragma once
#include <stdbool.h>

bool touchcal_load(void);  // carga y aplica la calibracion guardada; false si no hay
void touchcal_run(void);   // rutina de 4 cruces (bloqueante): calcula, aplica y guarda
