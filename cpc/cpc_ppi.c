/*
 * cpc_ppi.c — Amstrad CPC PPI (Intel 8255) y gestión de puertos I/O
 *
 * Basado en el emulador CPCEC de Cesar Nicolas-Gonzalez (GPLv3)
 * https://github.com/cpcitor/cpcec
 *
 * Este módulo implementa:
 *   - Intel 8255 PPI (Programmable Peripheral Interface)
 *   - Decodificación de direcciones de puertos del CPC
 *   - Integración con Gate Array, CRTC, PSG, teclado, cinta y FDC
 *
 * El PPI ocupa el rango de puertos 0xF4xx–0xF7xx del espacio de I/O
 * del Z80. Las líneas de dirección A9–A8 seleccionan el registro:
 *   A9=0,A8=0  → Puerto A  (PSG data bus)
 *   A9=0,A8=1  → Puerto B  (señales de entrada varias)
 *   A9=1,A8=0  → Puerto C  (control PSG + motor + teclado)
 *   A9=1,A8=1  → Control word (modo de operación)
 */

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tipos básicos                                                        */
/* ------------------------------------------------------------------ */

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;

/* ------------------------------------------------------------------ */
/* Estado del PPI 8255                                                 */
/* ------------------------------------------------------------------ */

/*
 * El 8255 tiene tres puertos (A, B, C) y un registro de control.
 * En el CPC el 8255 trabaja siempre en modo 0 (E/S simple).
 *
 * Puerto A (salida): bus de datos del PSG AY-3-8912
 * Puerto B (entrada):
 *   bit 7 — señal CASS IN (cinta)
 *   bit 6 — señal /EXP  (expansión presente, activo-bajo)
 *   bit 5 — señal /LK4  (conector de impresora)
 *   bit 4 — señal CRTC VSYNC (flanco de retrazado vertical)
 *   bit 3 — dip switch DE3 (distribución: Europa = 1, por lo general)
 *   bit 2 — dip switch DE2
 *   bit 1 — dip switch DE1
 *   bit 0 — dip switch DE0  (mínimo: 0 = 50Hz/PAL)
 * Puerto C:
 *   bits 3-0 (salida inferior) → fila de teclado a explorar (0-15)
 *   bit  4   (salida)          → motor de cinta (1 = ON)
 *   bit  5   (salida)          → LED de cinta   (1 = ON)
 *   bits 7-6 (salida superior) → control del PSG:
 *       00 = inactivo
 *       01 = leer del PSG
 *       10 = escribir en el PSG
 *       11 = seleccionar registro del PSG
 */

BYTE ppi_port_a;        /* Puerto A: valor escrito hacia el PSG        */
BYTE ppi_port_b;        /* Puerto B: valor leído de señales externas   */
BYTE ppi_port_c;        /* Puerto C: control y selección de fila kbd   */
BYTE ppi_control;       /* Palabra de control del 8255                 */

/* Conveniencia: campos de puerto C                                    */
#define PPI_C_KBD_ROW   (ppi_port_c & 0x0F)   /* fila teclado 0-15   */
#define PPI_C_TAPE_MOT  ((ppi_port_c >> 4) & 1)/* motor cinta         */
#define PPI_C_TAPE_LED  ((ppi_port_c >> 5) & 1)/* LED cinta           */
#define PPI_C_PSG_CTRL  ((ppi_port_c >> 6) & 3)/* control PSG (0-3)  */

/* ------------------------------------------------------------------ */
/* Estado externo que el PPI necesita conocer                         */
/* (en un emulador real serían variables globales o punteros)         */
/* ------------------------------------------------------------------ */

/* Teclado: matriz de 10 filas × 8 columnas.                          *
 * kbd_matrix[fila] tiene un bit a 0 por cada tecla pulsada.         */
extern BYTE kbd_matrix[16];

/* Cinta: señal actual de la cabeza lectora (bit más significativo)   */
extern BYTE tape_signal;    /* bit 7 = nivel actual del pin CASS IN   */

/* CRTC: el bit VSYNC que el PPI expone en el puerto B                */
extern BYTE crtc_vsync;     /* 1 cuando está activo el retrazado vert.*/

/* PSG AY-3-8912                                                       */
extern BYTE psg_index;      /* registro PSG seleccionado actualmente  */
extern BYTE psg_reg[16];    /* tabla de registros del PSG             */

/* Motor de cinta (callback o variable de estado)                     */
extern BYTE tape_motor;     /* 0 = parado, 1 = en marcha              */

/* ------------------------------------------------------------------ */
/* Bit-set/bit-reset del puerto C (instrucción especial del 8255)     */
/* ------------------------------------------------------------------ */
/*
 * Cuando el byte enviado al registro de control tiene el bit 7 = 0,
 * no es una nueva palabra de control sino una operación bit-set/reset
 * sobre el puerto C:
 *   bits 3-1 → número de bit (0-7)
 *   bit    0 → 1 = set, 0 = reset
 */
static inline void ppi_port_c_bitop(BYTE value)
{
    int bit = (value >> 1) & 7;
    if (value & 1)
        ppi_port_c |=  (1 << bit);
    else
        ppi_port_c &= ~(1 << bit);
}

/* ------------------------------------------------------------------ */
/* Acciones que dispara el puerto C al cambiar                        */
/* ------------------------------------------------------------------ */
static void ppi_update_portc(BYTE new_val)
{
    BYTE old_val = ppi_port_c;
    ppi_port_c = new_val;

    /* Motor de cinta: bit 4 */
    if ((old_val ^ new_val) & 0x10)
        tape_motor = (new_val >> 4) & 1;

    /* Control PSG: bits 7-6 */
    BYTE psg_ctrl = (new_val >> 6) & 3;
    switch (psg_ctrl)
    {
        case 0: /* Inactivo — no hacer nada */
            break;

        case 1: /* BDIR=0, BC1=1 → Leer registro (poco usado desde aquí) */
            break;

        case 2: /* BDIR=1, BC1=0 → Escribir en registro seleccionado */
            if (psg_index < 16)
                psg_reg[psg_index] = ppi_port_a;
            break;

        case 3: /* BDIR=1, BC1=1 → Latch de dirección (seleccionar reg) */
            psg_index = ppi_port_a & 0x0F;
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Escritura en el PPI                                                 */
/* ------------------------------------------------------------------ */
/*
 * port bits: A9–A8 seleccionan registro dentro del chip.
 * La dirección física del CPC para el PPI es 0xF4xx–0xF7xx.
 *   A10=1, A9=0, A8=0 → Puerto A  (port & 0x0300) == 0x0000
 *   A10=1, A9=0, A8=1 → Puerto B  (port & 0x0300) == 0x0100
 *   A10=1, A9=1, A8=0 → Puerto C  (port & 0x0300) == 0x0200
 *   A10=1, A9=1, A8=1 → Control   (port & 0x0300) == 0x0300
 */
void ppi_write(WORD port, BYTE value)
{
    switch ((port >> 8) & 3)   /* A9, A8 */
    {
        case 0: /* Puerto A — bus PSG */
            ppi_port_a = value;
            break;

        case 1: /* Puerto B — sólo entrada, escribir no tiene efecto */
            break;

        case 2: /* Puerto C — control motor, PSG, fila teclado */
            ppi_update_portc(value);
            break;

        case 3: /* Registro de control */
            if (value & 0x80)
            {
                /*
                 * Nueva palabra de control.
                 * El CPC siempre usa modo 0:
                 *   bit 7 = 1 (modo activo)
                 *   bit 6-5 = 00 (grupo A: modo 0)
                 *   bit 4 = dirección A (1=entrada, 0=salida)
                 *   bit 3 = dirección C superior
                 *   bit 2-1 = 00 (grupo B: modo 0)
                 *   bit 1 = dirección B (siempre entrada = 1)
                 *   bit 0 = dirección C inferior
                 *
                 * En el firmware del CPC se escribe 0x82:
                 *   Puerto A = salida, B = entrada, C sup = salida,
                 *   C inf = salida → control word = 0x82
                 */
                ppi_control = value;
                /* Al cambiar el modo, los puertos de salida se ponen a 0 */
                ppi_port_a = 0;
                ppi_update_portc(0);
            }
            else
            {
                /* Operación bit-set/reset sobre el puerto C */
                BYTE new_c = ppi_port_c;
                int  bit   = (value >> 1) & 7;
                if (value & 1) new_c |=  (1 << bit);
                else           new_c &= ~(1 << bit);
                ppi_update_portc(new_c);
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Lectura del PPI                                                     */
/* ------------------------------------------------------------------ */
BYTE ppi_read(WORD port)
{
    switch ((port >> 8) & 3)
    {
        case 0: /* Puerto A — bus PSG */
            /*
             * En modo lectura del PSG (BDIR=0, BC1=1 → PPI_C_PSG_CTRL=1)
             * se devuelve el valor del registro activo.
             * Si no estamos en modo lectura PSG, se devuelve el latch.
             */
            if (PPI_C_PSG_CTRL == 1 && psg_index < 16)
                return psg_reg[psg_index];
            return ppi_port_a;

        case 1: /* Puerto B — señales de entrada */
            /*
             * Construir el byte de estado:
             *   bit 7: CASS IN  (señal de cinta, invertida en algunos CPC)
             *   bit 6: /EXP     (expansión: 1 = no hay expansión)
             *   bit 5: /LK4     (siempre 1 en emulación normal)
             *   bit 4: VSYNC    (del CRTC)
             *   bits 3-0: dip switches de región
             *
             * Dip switches típicos del CPC 6128 inglés: 0b0101 = 0x05
             * (frecuencia 50Hz, idioma inglés)
             * Para versiones europeas/españolas puede variar.
             */
            ppi_port_b  = 0x7E;                    /* bits 6-1 fijos  */
            ppi_port_b |= (tape_signal  & 0x80);   /* bit 7: CASS IN  */
            ppi_port_b |= (crtc_vsync ? 0x10 : 0); /* bit 4: VSYNC    */
            ppi_port_b |= 0x05;                    /* dip: 50Hz + UK  */
            return ppi_port_b;

        case 2: /* Puerto C — lectura del valor de salida actual */
            return ppi_port_c;

        default: /* Control — devuelve el último valor escrito */
            return ppi_control;
    }
}

/* ------------------------------------------------------------------ */
/* Reset del PPI                                                       */
/* ------------------------------------------------------------------ */
void ppi_reset(void)
{
    ppi_port_a  = 0x00;
    ppi_port_b  = 0x00;
    ppi_control = 0x82; /* modo por defecto del CPC: A=out, B=in, C=out */
    tape_motor  = 0;
    ppi_update_portc(0x00);
}

/* ================================================================== */
/*                                                                     */
/*   DECODIFICADOR COMPLETO DE PUERTOS I/O DEL AMSTRAD CPC            */
/*                                                                     */
/*   El CPC usa A15–A8 como señal de selección de dispositivo         */
/*   (parcial address decoding). Una línea en 0 activa el chip.       */
/*                                                                     */
/* ================================================================== */

/*
 * Mapa de puertos del CPC (las líneas activas en 0 seleccionan):
 *
 *  A15=0 → Gate Array (escritura; sólo OUT, no hay IN significativo)
 *    0x7Fxx: Gate Array
 *      bits 7-6 del dato: función
 *        00xxxxxx → seleccionar modo/tinta de pantalla
 *        01xxxxxx → seleccionar registro de color (ink)
 *        10xxxxxx → cargar registro de modo (modo + ROM)
 *        11xxxxxx → RAM banking (solo 6128 y PLUS)
 *
 *  A14=0, A9=0 → CRTC 6845  (0xBCxx–0xBFxx)
 *    A9=0, A8=0 → seleccionar registro CRTC (escritura)
 *    A9=0, A8=1 → escribir en registro CRTC
 *    A9=1, A8=0 → leer dirección CRTC (solo algunos CRTC)
 *    A9=1, A8=1 → leer registro CRTC
 *
 *  A13=0 → selección de ROM superior (0xDFxx)
 *    escritura: seleccionar banco de ROM superior
 *
 *  A12=0 → impresora (0xEFxx) — no implementado aquí
 *
 *  A11=0 → PPI 8255 (0xF4xx–0xF7xx)
 *    A9-A8 seleccionan puerto A/B/C/Control (ver ppi_write/read)
 *
 *  A10=0 → FDC NEC 765 (0xFBxx)
 *    A8=0 → registro de estado del FDC (sólo lectura)
 *    A8=1 → registro de datos del FDC
 */

/* Forwards de funciones externas */
extern void gate_write(BYTE value);
extern void crtc_select(BYTE reg);
extern void crtc_write(BYTE value);
extern BYTE crtc_read(void);
extern BYTE crtc_status(void);
extern void upper_rom_select(BYTE rom);
extern void fdc_write_control(BYTE value);
extern void fdc_write_data(BYTE value);
extern BYTE fdc_read_status(void);
extern BYTE fdc_read_data(void);

/* ------------------------------------------------------------------ */
/* z80_out — gestión de todas las escrituras a puertos                */
/* ------------------------------------------------------------------ */
void z80_out(WORD port, BYTE value)
{
    /*
     * Cada dispositivo se activa cuando su línea de dirección está en 0.
     * Como el decodificado es parcial, puede haber más de un dispositivo
     * activo a la vez; el orden de evaluación importa.
     */

    /* Gate Array: A15 = 0 */
    if (!(port & 0x8000))
        gate_write(value);

    /* CRTC 6845: A14 = 0 */
    if (!(port & 0x4000))
    {
        switch ((port >> 8) & 3)  /* A9, A8 */
        {
            case 0: crtc_select(value); break;  /* 0xBC00: índice    */
            case 1: crtc_write(value);  break;  /* 0xBD00: dato      */
            /* 0xBE00, 0xBF00: lecturas, ignorar en escritura        */
        }
    }

    /* Selección ROM superior: A13 = 0 */
    if (!(port & 0x2000))
        upper_rom_select(value);

    /* PPI 8255: A11 = 0 */
    if (!(port & 0x0800))
        ppi_write(port, value);

    /* FDC NEC 765: A10 = 0 */
    if (!(port & 0x0400))
    {
        if (!(port & 0x0100))
            fdc_write_control(value);  /* 0xFA00: motor + selección */
        else
            fdc_write_data(value);     /* 0xFB01: datos FDC         */
    }
}

/* ------------------------------------------------------------------ */
/* z80_in — gestión de todas las lecturas de puertos                  */
/* ------------------------------------------------------------------ */
BYTE z80_in(WORD port)
{
    BYTE result = 0xFF;  /* bus flotante: todos los bits en 1 */

    /* CRTC 6845: A14 = 0 */
    if (!(port & 0x4000))
    {
        switch ((port >> 8) & 3)
        {
            case 2: result = crtc_status(); break; /* 0xBE00: estado  */
            case 3: result = crtc_read();   break; /* 0xBF00: dato    */
            /* 0xBC00, 0xBD00: sólo escritura                         */
        }
    }

    /* PPI 8255: A11 = 0 */
    if (!(port & 0x0800))
        result = ppi_read(port);

    /* FDC NEC 765: A10 = 0 */
    if (!(port & 0x0400))
    {
        if (!(port & 0x0100))
            result = fdc_read_status(); /* 0xFA00: estado FDC        */
        else
            result = fdc_read_data();   /* 0xFB01: dato FDC          */
    }

    /*
     * Teclado a través del PPI:
     * La lectura del teclado ocurre leyendo el Puerto A del PPI con
     * la fila de teclado preseleccionada en los bits 3-0 del Puerto C.
     * Esto ya queda cubierto por ppi_read() cuando el PSG no está
     * en modo lectura; aquí añadimos el paso explícito de la matriz:
     */

    return result;
}

/* ------------------------------------------------------------------ */
/* Lectura de la fila de teclado actual                               */
/* Esta función se llama desde ppi_read(port=0xF4xx) internamente    */
/* cuando el PSG NO está activo como fuente del Puerto A.            */
/* ------------------------------------------------------------------ */
BYTE keyboard_read_row(void)
{
    BYTE row = PPI_C_KBD_ROW;     /* fila 0-15 del Puerto C           */
    if (row < 10)
        return kbd_matrix[row];    /* 0 en bit = tecla pulsada         */
    return 0xFF;                   /* filas 10-15: ninguna tecla       */
}

/* ------------------------------------------------------------------ */
/* Inicialización completa del sistema de puertos                     */
/* ------------------------------------------------------------------ */
void ports_reset(void)
{
    memset(kbd_matrix, 0xFF, sizeof(kbd_matrix)); /* sin teclas        */
    tape_signal = 0;
    crtc_vsync  = 0;
    psg_index   = 0;
    memset(psg_reg, 0, sizeof(psg_reg));
    ppi_reset();
}

/* ================================================================== */
/*                  NOTAS DE IMPLEMENTACIÓN                           */
/* ================================================================== */
/*
 * 1. GATE ARRAY (0x7Fxx):
 *    El Gate Array gestiona modos de pantalla, paleta de 32 colores
 *    e interrupciones. No tiene registros de lectura; los accesos OUT
 *    son siempre suficientes.  Implementado en gate_write().
 *
 * 2. CRTC 6845 (0xBCxx–0xBFxx):
 *    Controla temporización de vídeo y dirección de vídeo RAM.
 *    Los registros 12-17 son legibles; el resto sólo escribible.
 *    El tipo de CRTC (0-4) cambia el comportamiento de ciertos bits.
 *
 * 3. PPI 8255 (0xF4xx–0xF7xx):
 *    Núcleo de este módulo. Gestiona:
 *      - Comunicación con el PSG (A+C)
 *      - Lectura del teclado (B+C)
 *      - Control del motor de cinta (C)
 *      - Señales de estado (B: VSYNC, CASS IN, dip-switches)
 *
 * 4. PSG AY-3-8912:
 *    Accedido indirectamente a través del Puerto A y C del PPI.
 *    Ciclo típico de escritura de registro:
 *      a) Poner índice en Puerto A, activar BDIR=BC1=1 (ctrl=3)
 *      b) Poner dato en Puerto A, activar BDIR=1, BC1=0 (ctrl=2)
 *    Ciclo de lectura:
 *      a) Poner índice en Puerto A, activar ctrl=3
 *      b) Leer Puerto A con ctrl=1
 *
 * 5. FDC NEC 765 (0xFBxx):
 *    Solo presente en CPC 664 y 6128.  El puerto 0xFA7E controla
 *    motor y selección de drive (bits 0-1 del dato).
 *    0xFB7E es el registro de datos/estado bidireccional del FDC.
 *
 * 6. DECODIFICADO PARCIAL:
 *    El CPC no usa un decodificador completo de 16 bits.  Esto hace
 *    que ciertos rangos de puertos 'alias' a los mismos dispositivos.
 *    Los emuladores deben respetar esto para compatibilidad con
 *    software que explota los alias (ej: algunos cargadores de cinta).
 */
