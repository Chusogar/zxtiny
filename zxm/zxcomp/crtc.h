/*
 * crtc.h - Motorola MC6845 CRTC emulation
 *
 * The CPC6128 uses a 6845 CRTC for video timing.
 * Key registers: R0=htotal, R1=hdisp, R2=hsync, R4=vtotal,
 *                R6=vdisp, R12/R13=display start address
 */

#ifndef CRTC_H
#define CRTC_H

#include <stdint.h>

typedef struct crtc_s {
    uint8_t  addr;           /* selected register */
    uint8_t  reg[18];        /* 18 registers */

    /* Internal counters */
    int      hcc;            /* horizontal character counter */
    int      vcc;            /* vertical character counter */
    int      sl;             /* scan line counter (within char row) */
    int      vsc;            /* vertical sync counter */
    int      hsc;            /* horizontal sync counter */

    /* Output signals */
    int      hsync_active;
    int      vsync_active;
    int      display_enabled;

    /* Current display address */
    uint16_t ma;             /* memory address */
    uint16_t ma_row_start;   /* address at start of row */

    /* Scanline count for gate array */
    int      scanline_count;
} crtc_t;

void     crtc_init        (crtc_t *crtc);
void     crtc_write       (crtc_t *crtc, int sel_data, uint8_t val);
uint8_t  crtc_read        (crtc_t *crtc, int sel_data);
int      crtc_scanline    (crtc_t *crtc);   /* advance one scanline, return 1 on VSYNC */
uint16_t crtc_get_line_addr(crtc_t *crtc, int line);

#endif /* CRTC_H */
