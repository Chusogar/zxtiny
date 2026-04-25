/*
 * crtc.c - MC6845 CRTC emulation
 */

#include "crtc.h"
#include <string.h>

/* Default register values for CPC6128 standard mode 1 */
static const uint8_t default_regs[18] = {
    63,  /* R0  Horizontal Total */
    40,  /* R1  Horizontal Displayed */
    46,  /* R2  Horizontal Sync Position */
    0x8E,/* R3  Sync widths (Vsync=8, Hsync=14) */
    38,  /* R4  Vertical Total */
    0,   /* R5  Vertical Total Adjust */
    25,  /* R6  Vertical Displayed */
    30,  /* R7  Vertical Sync Position */
    0,   /* R8  Interlace mode */
    7,   /* R9  Max Scan Line Address */
    0,   /* R10 Cursor Start */
    0,   /* R11 Cursor End */
    0x30,/* R12 Display Start Address (High) */
    0x00,/* R13 Display Start Address (Low) */
    0,   /* R14 Cursor Address (High) */
    0,   /* R15 Cursor Address (Low) */
    0,   /* R16 Light Pen (High) - read only */
    0,   /* R17 Light Pen (Low)  - read only */
};

void crtc_init(crtc_t *crtc)
{
    memset(crtc, 0, sizeof(*crtc));
    memcpy(crtc->reg, default_regs, sizeof(default_regs));
    crtc->display_enabled = 1;
    crtc->scanline_count  = 0;
}

void crtc_write(crtc_t *crtc, int sel_data, uint8_t val)
{
    if (sel_data == 0) {
        /* Address register select */
        crtc->addr = val & 0x1F;
    } else {
        /* Write to selected register */
        if (crtc->addr < 16) {
            crtc->reg[crtc->addr] = val;
        }
    }
}

uint8_t crtc_read(crtc_t *crtc, int sel_data)
{
    if (sel_data == 1) {
        /* Status / readable registers */
        switch (crtc->addr) {
        case 12: return crtc->reg[12];
        case 13: return crtc->reg[13];
        case 14: return crtc->reg[14];
        case 15: return crtc->reg[15];
        case 16: return crtc->reg[16];
        case 17: return crtc->reg[17];
        default: return 0;
        }
    }
    /* Status register (type 0/1 CRTC) */
    return (crtc->vsync_active ? 0x20 : 0x00);
}

/*
 * Advance the CRTC by one scanline (256 T-states at 4MHz).
 * Returns 1 when VSYNC starts, 0 otherwise.
 */
int crtc_scanline(crtc_t *crtc)
{
    int vsync_start = 0;

    crtc->scanline_count++;

    /* Horizontal: one full character line per "scanline" in our model */
    crtc->hcc++;
    if (crtc->hcc > crtc->reg[0]) {
        crtc->hcc = 0;

        /* Advance scan line within character row */
        crtc->sl++;
        if (crtc->sl > crtc->reg[9]) {
            crtc->sl = 0;

            /* Advance vertical character counter */
            crtc->ma_row_start = crtc->ma;
            crtc->vcc++;

            if (crtc->vcc == crtc->reg[7]) {
                /* VSYNC starts */
                crtc->vsync_active = 1;
                crtc->vsc = 0;
                vsync_start = 1;
                crtc->scanline_count = 0;
            }

            if (crtc->vcc > crtc->reg[4]) {
                /* Vertical total reached -> new frame */
                crtc->vcc = 0;
                /* Reload display start address */
                crtc->ma = ((uint16_t)(crtc->reg[12] & 0x3F) << 8) |
                             crtc->reg[13];
                crtc->ma_row_start = crtc->ma;
                crtc->vsync_active = 0;
            }
        } else {
            /* Same character row, next scan line: restore MA to row start */
            crtc->ma = crtc->ma_row_start;
        }
    }

    /* VSYNC counter */
    if (crtc->vsync_active) {
        crtc->vsc++;
        if (crtc->vsc >= ((crtc->reg[3] >> 4) & 0xF)) {
            crtc->vsync_active = 0;
        }
    }

    /* HSYNC */
    if (crtc->hcc == crtc->reg[2]) {
        crtc->hsync_active = 1;
        crtc->hsc = 0;
    }
    if (crtc->hsync_active) {
        crtc->hsc++;
        if (crtc->hsc >= (crtc->reg[3] & 0xF)) {
            crtc->hsync_active = 0;
        }
    }

    /* Display enable: inside horizontal and vertical displayed area */
    crtc->display_enabled = (crtc->hcc < crtc->reg[1]) &&
                            (crtc->vcc < crtc->reg[6]);

    return vsync_start;
}

/*
 * Get the video memory address for a given output scanline.
 * The CRTC interleaves scan lines within character rows.
 */
uint16_t crtc_get_line_addr(crtc_t *crtc, int line)
{
    /* CPC: MA is updated as CRTC scans, but for rendering
     * we compute directly from display start + line */
    int chars_per_row = crtc->reg[1];
    int max_scanline  = crtc->reg[9] + 1;
    int char_row      = line / max_scanline;
    int scan_in_row   = line % max_scanline;

    /* Base display start address */
    uint16_t base = ((uint16_t)(crtc->reg[12] & 0x3F) << 8) | crtc->reg[13];

    /* Each character row: chars_per_row bytes, but in CPC the MA wraps in
     * a specific way. For mode 1: addr = base + char_row*80 + scan_in_row*2048 */
    /* CPC video address: MA bits shuffled via Gate Array */
    /* Actual CPC: addr[15:14]=from MA[15:14], addr[13]=ra[0], addr[12:9]=MA[12:9] etc */
    /* Simplified correct formula: */
    uint16_t addr = base + (char_row * chars_per_row) + (scan_in_row * 0x800);
    return addr & 0x3FFF;  /* 16K VRAM window */
}
