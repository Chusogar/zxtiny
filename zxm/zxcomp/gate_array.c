/*
 * gate_array.c - Amstrad CPC Gate Array implementation
 */

#include "gate_array.h"
#include "crtc.h"
#include "memory.h"
#include <string.h>

/*
 * The CPC has a 27-colour hardware palette.
 * Each hardware colour index maps to an RGB value.
 * Based on the official CPC hardware documentation.
 */
static const uint32_t cpc_hw_colours[27] = {
    0x808080, /* 0  - White */
    0x808080, /* 1  - White (same) */
    0x00FF80, /* 2  - Sea Green */
    0xFFFF80, /* 3  - Pastel Yellow */
    0x000080, /* 4  - Blue */
    0xFF0080, /* 5  - Purple */
    0x008080, /* 6  - Cyan */
    0xFF8080, /* 7  - Pink */
    0xFF0080, /* 8  - Purple */
    0xFFFF80, /* 9  - Pastel Yellow */
    0xFFFF00, /* 10 - Bright Yellow */
    0xFFFFFF, /* 11 - Bright White */
    0xFF0000, /* 12 - Bright Red */
    0xFF00FF, /* 13 - Bright Magenta */
    0xFF8000, /* 14 - Orange */
    0xFF80FF, /* 15 - Pastel Magenta */
    0x000080, /* 16 - Blue */
    0x00FF80, /* 17 - Sea Green */
    0x00FF00, /* 18 - Bright Green */
    0x00FFFF, /* 19 - Bright Cyan */
    0x000000, /* 20 - Black */
    0x0000FF, /* 21 - Bright Blue */
    0x008000, /* 22 - Green */
    0x0080FF, /* 23 - Sky Blue */
    0x800080, /* 24 - Magenta */
    0x8000FF, /* 25 - Pastel Blue */
    0x800000, /* 26 - Red */
};

/* Default palette: standard CPC power-on colours */
static const uint8_t default_palette[17] = {
    /* pen 0..15: hardware colour index */
    20, 24, 20, 12, 20, 26, 20, 18,
    20, 6,  20, 14, 20, 10, 20, 22,
    /* border = pen 16 */ 20
};

void gate_array_init(gate_array_t *ga)
{
    memset(ga, 0, sizeof(*ga));
    for (int i = 0; i < 27; i++)
        ga->hw_palette[i] = cpc_hw_colours[i];
    for (int i = 0; i < 17; i++)
        ga->pen_colour[i] = default_palette[i];
    ga->mode = 1; /* mode 1 at startup */
    ga->pen_sel = 0;
}

/*
 * Gate Array write:
 * Bits 7:6 = function
 *   00: Select pen           (bits 4:0 = pen, bit4=1 -> border)
 *   01: Set colour           (bits 4:0 = hw colour index)
 *   10: Mode + ROM config    (bits 1:0=mode, bit2=upper_dis, bit3=lower_dis, bit4=int_delay)
 *   11: RAM banking          (handled in memory.c via gate array call)
 */
void gate_array_write(gate_array_t *ga, mem_t *mem, uint8_t val)
{
    switch ((val >> 6) & 3) {
    case 0: /* Select pen */
        ga->pen_sel = (val & 0x10) ? 16 : (val & 0x0F);
        break;
    case 1: /* Set colour */
        ga->pen_colour[ga->pen_sel] = val & 0x1F;
        break;
    case 2: /* Mode + ROM config */
        ga->mode = val & 3;
        mem->lower_rom_en = !(val & 0x04);
        mem->upper_rom_en = !(val & 0x08);
        mem_update_map(mem);
        ga->int_delay = (val >> 4) & 1;
        break;
    case 3: /* RAM banking */
        mem->ram_config = val & 7;
        mem_update_map(mem);
        break;
    }
}

/* ── Pixel rendering ──────────────────────────────────────────────────── */

/*
 * Mode 0: 160 pixels/line, 4bpp (16 colours)
 * Each byte encodes 2 pixels using bits:
 *   pixel0: b7,b5,b3,b1
 *   pixel1: b6,b4,b2,b0
 */
static inline uint8_t mode0_pixel(uint8_t byte, int px)
{
    /* px=0 or px=1 */
    uint8_t b = byte;
    if (px == 0) {
        return ((b>>7)&1) | (((b>>5)&1)<<1) | (((b>>3)&1)<<2) | (((b>>1)&1)<<3);
    } else {
        return ((b>>6)&1) | (((b>>4)&1)<<1) | (((b>>2)&1)<<2) | (((b>>0)&1)<<3);
    }
}

/*
 * Mode 1: 320 pixels/line, 2bpp (4 colours)
 * Each byte encodes 4 pixels:
 *   pixel0: b7,b3
 *   pixel1: b6,b2
 *   pixel2: b5,b1
 *   pixel3: b4,b0
 */
static inline uint8_t mode1_pixel(uint8_t byte, int px)
{
    return (((byte >> (7-px)) & 1) | (((byte >> (3-px)) & 1) << 1));
}

/*
 * Mode 2: 640 pixels/line, 1bpp (2 colours)
 * Each byte encodes 8 pixels: bit7=px0 ... bit0=px7
 */
static inline uint8_t mode2_pixel(uint8_t byte, int px)
{
    return (byte >> (7-px)) & 1;
}

/*
 * Render one scanline to the framebuffer.
 * CPC visible area: ~768 pixels wide, 544 lines tall
 * CRTC defines the video area; gate array clips/scales.
 */
void gate_array_scanline(gate_array_t *ga, crtc_t *crtc, mem_t *mem, int line)
{
    if (line < 0 || line >= GA_SCREEN_H) return;

    uint32_t *dst = ga->framebuffer + line * GA_SCREEN_W;
    uint32_t  border_col = ga->hw_palette[ga->pen_colour[16]];

    /* Check if this scanline is in active display area */
    if (!crtc->display_enabled || crtc->vsync_active || crtc->hsync_active) {
        /* Fill with border */
        for (int x = 0; x < GA_SCREEN_W; x++)
            dst[x] = border_col;
        return;
    }

    /* Video memory address from CRTC */
    uint16_t video_addr = crtc_get_line_addr(crtc, line);

    /* Each CPC screen line is 80 bytes wide in mode 0, 80 in mode1, 80 in mode2 */
    /* Display is 640 pixels wide: mode0=320px, mode1=640px, mode2=1280px (scaled) */

    int bytes_per_line = 80;
    uint16_t ma = video_addr;

    /* Left border */
    int border_left = (GA_SCREEN_W - 640) / 2;
    for (int x = 0; x < border_left; x++)
        dst[x] = border_col;

    /* Active pixels */
    int px = border_left;
    switch (ga->mode) {
    case 0: /* 160x200, 2 pixels/byte */
        for (int b = 0; b < bytes_per_line && px < GA_SCREEN_W - border_left; b++) {
            uint8_t byte = mem_read(mem, ma & 0xFFFF);
            ma++;
            for (int p = 0; p < 2 && px < GA_SCREEN_W; p++) {
                uint8_t pen = mode0_pixel(byte, p);
                uint32_t col = ga->hw_palette[ga->pen_colour[pen & 0xF]];
                /* Mode 0: each pixel is 4 screen pixels wide */
                for (int s = 0; s < 4 && px < GA_SCREEN_W; s++)
                    dst[px++] = col;
            }
        }
        break;
    case 1: /* 320x200, 4 pixels/byte */
        for (int b = 0; b < bytes_per_line && px < GA_SCREEN_W - border_left; b++) {
            uint8_t byte = mem_read(mem, ma & 0xFFFF);
            ma++;
            for (int p = 0; p < 4 && px < GA_SCREEN_W; p++) {
                uint8_t pen = mode1_pixel(byte, p);
                uint32_t col = ga->hw_palette[ga->pen_colour[pen & 3]];
                /* Mode 1: each pixel is 2 screen pixels wide */
                for (int s = 0; s < 2 && px < GA_SCREEN_W; s++)
                    dst[px++] = col;
            }
        }
        break;
    case 2: /* 640x200, 8 pixels/byte */
        for (int b = 0; b < bytes_per_line && px < GA_SCREEN_W - border_left; b++) {
            uint8_t byte = mem_read(mem, ma & 0xFFFF);
            ma++;
            for (int p = 0; p < 8 && px < GA_SCREEN_W; p++) {
                uint8_t pen = mode2_pixel(byte, p);
                uint32_t col = ga->hw_palette[ga->pen_colour[pen & 1]];
                dst[px++] = col;
            }
        }
        break;
    default:
        break;
    }

    /* Right border */
    while (px < GA_SCREEN_W)
        dst[px++] = border_col;
}
