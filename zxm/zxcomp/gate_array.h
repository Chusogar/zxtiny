/*
 * gate_array.h - Amstrad CPC Gate Array (video controller)
 *
 * Handles:
 *  - Colour palette (16 colour pens from 27-colour hardware palette)
 *  - Screen modes (0=160x200/16col, 1=320x200/4col, 2=640x200/2col)
 *  - ROM banking (lower/upper ROM enable)
 *  - Interrupt timing
 *  - Scanline rendering to RGB888 framebuffer
 */

#ifndef GATE_ARRAY_H
#define GATE_ARRAY_H

#include <stdint.h>
#include "memory.h"
#include "crtc.h"

#define GA_SCREEN_W  768
#define GA_SCREEN_H  544

typedef struct gate_array_s {
    /* Registers */
    uint8_t  pen_sel;           /* selected pen index (0-15, 16=border) */
    uint8_t  pen_colour[17];    /* pen -> hardware colour index */
    uint8_t  mode;              /* 0/1/2/3 */
    uint8_t  rom_cfg;           /* bit0=lower_rom_dis, bit1=upper_rom_dis */
    uint8_t  int_delay;

    /* 27-colour hardware palette: RGB888 */
    uint32_t hw_palette[27];

    /* Output framebuffer (RGBX32, 768x544) */
    uint32_t framebuffer[GA_SCREEN_W * GA_SCREEN_H];

    int      scanline;          /* current render scanline */
} gate_array_t;

void gate_array_init    (gate_array_t *ga);
void gate_array_write   (gate_array_t *ga, mem_t *mem, uint8_t val);
void gate_array_scanline(gate_array_t *ga, crtc_t *crtc, mem_t *mem, int line);

#endif /* GATE_ARRAY_H */
