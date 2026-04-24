/*
 * cpc_ppi.h — Amstrad CPC: PPI 8255 y gestión de puertos I/O
 *
 * Basado en el emulador CPCEC de Cesar Nicolas-Gonzalez (GPLv3)
 * https://github.com/cpcitor/cpcec
 */

#ifndef CPC_PPI_H
#define CPC_PPI_H

#include <stdint.h>

typedef uint8_t  BYTE;
typedef uint16_t WORD;

/* ------------------------------------------------------------------ */
/* Estado del PPI 8255 (variables globales)                           */
/* ------------------------------------------------------------------ */

extern BYTE ppi_port_a;     /* Puerto A: bus de datos PSG (salida)     */
extern BYTE ppi_port_b;     /* Puerto B: señales externas (entrada)    */
extern BYTE ppi_port_c;     /* Puerto C: control PSG + motor + kbd     */
extern BYTE ppi_control;    /* Palabra de control del 8255             */

/* ------------------------------------------------------------------ */
/* Macros de conveniencia para Puerto C                               */
/* ------------------------------------------------------------------ */

/** Fila de teclado seleccionada (bits 3-0 de Puerto C) */
#define PPI_C_KBD_ROW    (ppi_port_c & 0x0F)

/** Motor de cinta: bit 4 del Puerto C */
#define PPI_C_TAPE_MOT   ((ppi_port_c >> 4) & 1)

/** LED de cinta: bit 5 del Puerto C */
#define PPI_C_TAPE_LED   ((ppi_port_c >> 5) & 1)

/**
 * Control PSG: bits 7-6 del Puerto C.
 *   0 = inactivo
 *   1 = leer registro PSG
 *   2 = escribir en registro PSG
 *   3 = seleccionar registro PSG (latch dirección)
 */
#define PPI_C_PSG_CTRL   ((ppi_port_c >> 6) & 3)

/* ------------------------------------------------------------------ */
/* Constantes de control 8255                                         */
/* ------------------------------------------------------------------ */

/** Palabra de control por defecto del CPC (A=salida, B=entrada, C=salida) */
#define PPI_CTRL_DEFAULT  0x82

/* ------------------------------------------------------------------ */
/* API pública                                                         */
/* ------------------------------------------------------------------ */

/**
 * ppi_reset() — Reinicia el PPI al estado de encendido.
 * Establece ppi_control = 0x82 y pone todos los puertos a 0.
 */
void ppi_reset(void);

/**
 * ppi_write(port, value) — Escribe en el PPI.
 * @port:  dirección completa del Z80 (se usan bits A9-A8)
 * @value: byte a escribir
 *
 * Decodificación:
 *   (port>>8)&3 == 0 → Puerto A (bus PSG)
 *   (port>>8)&3 == 1 → Puerto B (ignorado, sólo entrada)
 *   (port>>8)&3 == 2 → Puerto C (control motor/PSG/teclado)
 *   (port>>8)&3 == 3 → Registro de control (o bit-set/reset si bit7=0)
 */
void ppi_write(WORD port, BYTE value);

/**
 * ppi_read(port) — Lee del PPI.
 * @port:  dirección completa del Z80 (se usan bits A9-A8)
 * @return byte leído
 *
 * Puerto A: valor PSG si PSG_CTRL==1, o latch ppi_port_a.
 * Puerto B: ensambla señales externas en tiempo real.
 * Puerto C: devuelve ppi_port_c actual.
 * Control:  devuelve ppi_control.
 */
BYTE ppi_read(WORD port);

/**
 * z80_out(port, value) — Decodificador completo de escritura de puertos.
 * Enruta la escritura al Gate Array, CRTC, ROM superior, PPI o FDC
 * según las líneas de dirección A15–A8.
 */
void z80_out(WORD port, BYTE value);

/**
 * z80_in(port) — Decodificador completo de lectura de puertos.
 * @return byte leído del dispositivo correspondiente, o 0xFF (bus float).
 */
BYTE z80_in(WORD port);

/**
 * keyboard_read_row() — Devuelve el estado de la fila activa del teclado.
 * La fila se toma de PPI_C_KBD_ROW (bits 3-0 de Puerto C).
 * @return byte con bit=0 por cada tecla pulsada en esa fila.
 */
BYTE keyboard_read_row(void);

/**
 * ports_reset() — Inicializa todos los periféricos a su estado de reset.
 */
void ports_reset(void);

/* ------------------------------------------------------------------ */
/* Señales externas que el PPI consume (deben definirse externamente)  */
/* ------------------------------------------------------------------ */

/** Matriz de teclado: kbd_matrix[fila], bit=0 → tecla pulsada */
extern BYTE kbd_matrix[16];

/** Señal de cinta: bit 7 = nivel CASS IN actual */
extern BYTE tape_signal;

/** Señal VSYNC del CRTC: 1 durante el retrazado vertical */
extern BYTE crtc_vsync;

/** Índice de registro PSG actualmente seleccionado */
extern BYTE psg_index;

/** Tabla de registros del PSG AY-3-8912 */
extern BYTE psg_reg[16];

/** Estado del motor de cinta: 0=parado, 1=en marcha */
extern BYTE tape_motor;

#endif /* CPC_PPI_H */
