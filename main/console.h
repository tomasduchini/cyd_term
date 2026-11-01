#pragma once
#include <stdbool.h>

// Control dentro del texto: con_type(CON_C_HI "hola" CON_C_TEXT " mundo\n")
#define CON_C_TEXT   "\x01"
#define CON_C_HI     "\x02"
#define CON_C_WARN   "\x03"
#define CON_C_DIM    "\x04"
#define CON_CLEAR    "\x05"   // limpia y vuelve el cursor arriba
#define CON_PAUSE_1S "\x06"   // pausa ~1 s
#define CON_EVENT    "\x07"   // marca un evento que se lee con con_take_event()

void con_init(void);
void con_type(const char *s);  // encola texto (un solo escritor)
void con_step(void);           // llamar siempre: consume la cola aunque no este visible
void con_show(void);           // repinta desde el buffer y pasa a visible
void con_hide(void);
bool con_take_event(void);     // true una vez por cada CON_EVENT procesado
