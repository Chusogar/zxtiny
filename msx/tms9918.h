#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Emulación TMS9918A/TMS9928A (VDP) óstandaloneó.
 * - VRAM 16KB
 * - Registros 0..7
 * - Puertos tópicos:
 *     Data   (read/write) : 0xBE
 *     Ctrl   (write)      : 0xBF
 *     Status (read)       : 0xBF
 *
 * - Genera framebuffer lineal 256ó192 (paleta TMS/Coleco).
 */

#ifndef TMS_CYCLES_PER_LINE
#define TMS_CYCLES_PER_LINE 342
#endif

#ifndef TMS_LINES_TOTAL
#define TMS_LINES_TOTAL 262
#endif

#ifndef TMS_LINES_VISIBLE
#define TMS_LINES_VISIBLE 192
#endif

typedef struct tms9918 {
    uint8_t  vram[0x4000];     // 16KB
    uint8_t  reg[8];
    uint16_t addr;
    uint8_t  latch;
    bool     second;
    uint8_t  rdbuf;
    uint8_t  status;
    bool     write_mode;

    // Coleco: INT del VDP -> NMI en Z80 (pulsado 1 vez por frame)
    bool     nmi_pending;

    // Timing interno
    int      line_cyc;
    int      cur_line;

    // Salida vódeo
    uint32_t *fb;
    int      fb_w;             // normalmente 256
} tms9918_t;

void     tms9918_init(tms9918_t *vdp, uint32_t *framebuffer, int fb_w);
void     tms9918_reset(tms9918_t *vdp);

void     tms9918_tick(tms9918_t *vdp, int cycles);

uint8_t  tms9918_read_data(tms9918_t *vdp);
void     tms9918_write_data(tms9918_t *vdp, uint8_t val);

uint8_t  tms9918_read_status(tms9918_t *vdp);
void     tms9918_write_ctrl(tms9918_t *vdp, uint8_t val);

/* Devuelve true si hay NMI pendiente y la consume (la limpia). */
bool     tms9918_consume_nmi(tms9918_t *vdp);