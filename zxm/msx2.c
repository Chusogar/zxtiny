#include "msx2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static MSX2 msx;

// =============================================================================
// Paleta V9938 por defecto (TMS9918 compatible, valores 3-bit → ARGB8888)
// =============================================================================

static const uint8_t v9938_default_pal[16][3] = { // R,G,B (3-bit each)
    {0,0,0},{0,0,0},{1,6,1},{3,7,3},{1,1,7},{2,3,7},{5,1,1},{2,6,7},
    {7,1,1},{7,3,3},{6,6,1},{6,7,4},{1,4,1},{6,2,5},{5,5,5},{7,7,7}
};

static uint32_t rgb3_to_argb(uint8_t r3, uint8_t g3, uint8_t b3) {
    uint8_t r = (r3 << 5) | (r3 << 2) | (r3 >> 1);
    uint8_t g = (g3 << 5) | (g3 << 2) | (g3 >> 1);
    uint8_t b = (b3 << 5) | (b3 << 2) | (b3 >> 1);
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// G7 (Screen 8) fixed 256-color palette: 3-3-2 bit RGB
static uint32_t g7_color(uint8_t c) {
    uint8_t r = (c >> 5) & 7;
    uint8_t g = (c >> 2) & 7;
    uint8_t b = c & 3;
    r = (r << 5) | (r << 2) | (r >> 1);
    g = (g << 5) | (g << 2) | (g >> 1);
    b = (b << 6) | (b << 4) | (b << 2) | b;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// =============================================================================
// V9938 VDP - Modo de pantalla
// =============================================================================

// Screen mode from mode bits M1-M5
static int vdp_screen_mode(MSX2_VDP* v) {
    int m1 = (v->regs[1] >> 4) & 1;
    int m2 = (v->regs[1] >> 3) & 1;
    int m3 = (v->regs[0] >> 1) & 1;
    int m4 = (v->regs[0] >> 2) & 1;
    int m5 = (v->regs[0] >> 3) & 1;
    int bits = (m5 << 4) | (m4 << 3) | (m3 << 2) | (m2 << 1) | m1;
    switch (bits) {
    case 0x01: return 0;   // T1  (Screen 0, 40 col)
    case 0x09: return 10;  // T2  (Screen 0, 80 col)
    case 0x00: return 1;   // G1  (Screen 1)
    case 0x04: return 2;   // G2  (Screen 2)
    case 0x02: return 3;   // MC  (Screen 3)
    case 0x06: return 4;   // G3  (Screen 4)
    case 0x08: return 5;   // G4  (Screen 5)
    case 0x0C: return 6;   // G5  (Screen 6)
    case 0x10: return 7;   // G6  (Screen 7)
    case 0x18: return 8;   // G7  (Screen 8)
    default:   return 1;
    }
}

// Active lines: 192 or 212
static int vdp_active_lines(MSX2_VDP* v) {
    return (v->regs[9] & 0x80) ? 212 : 192;
}

// =============================================================================
// V9938 VDP - VRAM access helpers
// =============================================================================

static inline uint8_t vram_rd(MSX2_VDP* v, uint32_t addr) {
    return v->vram[addr & (VDP_VRAM_SIZE - 1)];
}

static inline void vram_wr(MSX2_VDP* v, uint32_t addr, uint8_t val) {
    v->vram[addr & (VDP_VRAM_SIZE - 1)] = val;
}

// =============================================================================
// V9938 VDP - Scanline rendering
// =============================================================================

// Render T1 (Screen 0, 40-column text)
static void vdp_render_t1(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t nt = (uint32_t)(v->regs[2] & 0x7F) << 10; // name table
    uint32_t pg = (uint32_t)(v->regs[4] & 0x3F) << 11; // pattern gen
    uint8_t  fg_idx = (v->regs[7] >> 4) & 0x0F;
    uint8_t  bg_idx = v->regs[7] & 0x0F;
    uint32_t fg = v->palette[fg_idx ? fg_idx : 0];
    uint32_t bg = v->palette[bg_idx ? bg_idx : 0];
    int row = line / 8;
    int ymod = line & 7;
    // 8-pixel left border, 240 pixels text (40 chars × 6), 8-pixel right border
    for (int x = 0; x < MSX_SCREEN_W; x++) buf[x] = bg;
    for (int col = 0; col < 40; col++) {
        uint8_t ch = vram_rd(v, nt + row * 40 + col);
        uint8_t pat = vram_rd(v, pg + ch * 8 + ymod);
        int px = 8 + col * 6;
        for (int bit = 0; bit < 6; bit++) {
            if (px + bit < MSX_SCREEN_W)
                buf[px + bit] = (pat & (0x80 >> bit)) ? fg : bg;
        }
    }
}

// Render G1 (Screen 1, 32×24 tiles)
static void vdp_render_g1(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t nt = (uint32_t)(v->regs[2] & 0x7F) << 10;
    uint32_t ct = (uint32_t)(v->regs[3]) << 6;
    uint32_t pg = (uint32_t)(v->regs[4] & 0x3F) << 11;
    int row = line / 8;
    int ymod = line & 7;
    for (int col = 0; col < 32; col++) {
        uint8_t ch = vram_rd(v, nt + row * 32 + col);
        uint8_t pat = vram_rd(v, pg + ch * 8 + ymod);
        uint8_t clr = vram_rd(v, ct + (ch >> 3));
        uint8_t fg_i = (clr >> 4) & 0x0F;
        uint8_t bg_i = clr & 0x0F;
        uint32_t fg = v->palette[fg_i ? fg_i : 0];
        uint32_t bg = v->palette[bg_i ? bg_i : 0];
        int px = col * 8;
        for (int bit = 0; bit < 8; bit++)
            buf[px + bit] = (pat & (0x80 >> bit)) ? fg : bg;
    }
}

// Render G2/G3 (Screen 2/4, high-res tiles)
static void vdp_render_g2(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t nt = (uint32_t)(v->regs[2] & 0x7F) << 10;
    uint32_t ct_base = (uint32_t)(v->regs[3] & 0x80) << 6;
    uint32_t pg_base = (uint32_t)(v->regs[4] & 0x04) << 11;
    uint16_t ct_mask = ((uint16_t)(v->regs[3] & 0x7F) << 3) | 0x07;
    uint16_t pg_mask = ((uint16_t)(v->regs[4] & 0x03) << 8) | 0xFF;
    // Never mask off bits in practice for most Spectrum-like games: both are 0x1FFF
    int row = line / 8;
    int ymod = line & 7;
    int third = (line / 64) * 256; // 0, 256, or 512
    for (int col = 0; col < 32; col++) {
        uint8_t ch = vram_rd(v, nt + row * 32 + col);
        uint16_t idx = ((ch + third) * 8 + ymod);
        uint8_t pat = vram_rd(v, pg_base + (idx & (pg_mask * 8 + 7)));
        uint8_t clr = vram_rd(v, ct_base + (idx & (ct_mask * 8 + 7)));
        // Correct masking for G2: pattern index & mask
        pat = vram_rd(v, pg_base + (idx & ((pg_mask << 3) | 7)));
        clr = vram_rd(v, ct_base + (idx & ((ct_mask << 3) | 7)));
        uint8_t fg_i = (clr >> 4) & 0x0F;
        uint8_t bg_i = clr & 0x0F;
        uint32_t fg = v->palette[fg_i ? fg_i : 0];
        uint32_t bg = v->palette[bg_i ? bg_i : 0];
        int px = col * 8;
        for (int bit = 0; bit < 8; bit++)
            buf[px + bit] = (pat & (0x80 >> bit)) ? fg : bg;
    }
}

// Render MC (Screen 3, multicolor)
static void vdp_render_mc(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t nt = (uint32_t)(v->regs[2] & 0x7F) << 10;
    uint32_t pg = (uint32_t)(v->regs[4] & 0x3F) << 11;
    int row = line / 8;
    int sub = (line / 4) & 1;
    for (int col = 0; col < 32; col++) {
        uint8_t ch = vram_rd(v, nt + row * 32 + col);
        uint8_t clr = vram_rd(v, pg + ch * 8 + sub * 2 + ((row & 3) >= 2 ? 1 : 0));
        // Hmm, MC addressing: pattern data at pg + ch*8 + (line/4)%2 * ... 
        // Actually: each char gives 2 colors per 4-pixel-high block
        clr = vram_rd(v, pg + ch * 8 + ((line >> 2) & 7));
        uint8_t hi = (clr >> 4) & 0x0F;
        uint8_t lo = clr & 0x0F;
        uint32_t c1 = v->palette[hi ? hi : 0];
        uint32_t c2 = v->palette[lo ? lo : 0];
        int px = col * 8;
        for (int x = 0; x < 4; x++) buf[px + x] = c1;
        for (int x = 4; x < 8; x++) buf[px + x] = c2;
    }
}

// Render G4 (Screen 5, 256×212 bitmap, 4bpp)
static void vdp_render_g4(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
    uint32_t addr = base + (uint32_t)line * 128;
    for (int x = 0; x < 256; x += 2) {
        uint8_t byte = vram_rd(v, addr++);
        buf[x]     = v->palette[(byte >> 4) & 0x0F];
        buf[x + 1] = v->palette[byte & 0x0F];
    }
}

// Render G5 (Screen 6, 512×212 bitmap, 2bpp → display as 256 wide)
static void vdp_render_g5(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
    uint32_t addr = base + (uint32_t)line * 128;
    for (int x = 0; x < 256; x += 2) {
        uint8_t byte = vram_rd(v, addr++);
        // 4 pixels per byte, 2 bits each; take pixels 0 and 2 for 2:1 downscale
        buf[x]     = v->palette[(byte >> 6) & 3];
        buf[x + 1] = v->palette[(byte >> 2) & 3];
    }
}

// Render G6 (Screen 7, 512×212 bitmap, 4bpp → display as 256 wide)
static void vdp_render_g6(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t base = (uint32_t)(v->regs[2] & 0x20) << 11;
    uint32_t addr = base + (uint32_t)line * 256;
    for (int x = 0; x < 256; x++) {
        uint8_t byte = vram_rd(v, addr);
        // Two 4bpp pixels at 512 width → we take the left one
        buf[x] = v->palette[(byte >> 4) & 0x0F];
        addr++;
    }
}

// Render G7 (Screen 8, 256×212 bitmap, 8bpp, fixed 3-3-2 palette)
static void vdp_render_g7(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
    uint32_t addr = base + (uint32_t)line * 256;
    for (int x = 0; x < 256; x++)
        buf[x] = g7_color(vram_rd(v, addr++));
}

// Render sprites mode 1 (Screen 1-3: 8/16 px, 4 per line, 1 color)
static void vdp_render_sprites_m1(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t sat = (uint32_t)(v->regs[5] & 0x7F) << 7;
    uint32_t spg = (uint32_t)(v->regs[6] & 0x07) << 11;
    int size = (v->regs[1] & 0x02) ? 16 : 8;
    int mag  = (v->regs[1] & 0x01) ? 2 : 1;
    int drawn = 0;
    for (int i = 0; i < 32 && drawn < 4; i++) {
        int y = vram_rd(v, sat + i * 4);
        if (y == 208) break;
        y = (y + 1) & 0xFF;
        if (line < y || line >= y + size * mag) continue;
        int x    = vram_rd(v, sat + i * 4 + 1);
        int pat  = vram_rd(v, sat + i * 4 + 2);
        int attr = vram_rd(v, sat + i * 4 + 3);
        if (attr & 0x80) x -= 32;
        int clr = attr & 0x0F;
        if (clr == 0) { drawn++; continue; }
        uint32_t color = v->palette[clr];
        if (size == 16) pat &= 0xFC;
        int sy = (line - y) / mag;
        for (int sx = 0; sx < size; sx++) {
            int bx = sx;
            int by = sy;
            uint8_t bits;
            if (size == 16) {
                int quad = (bx >= 8 ? 1 : 0) + (by >= 8 ? 2 : 0);
                bits = vram_rd(v, spg + (pat + quad) * 8 + (by & 7));
                bx &= 7;
            } else {
                bits = vram_rd(v, spg + pat * 8 + by);
            }
            if (bits & (0x80 >> bx)) {
                for (int m = 0; m < mag; m++) {
                    int px = x + sx * mag + m;
                    if (px >= 0 && px < 256) buf[px] = color;
                }
            }
        }
        drawn++;
    }
}

// Render sprites mode 2 (Screen 4+: 8/16 px, 8 per line, color per line)
static void vdp_render_sprites_m2(MSX2_VDP* v, int line, uint32_t* buf) {
    uint32_t sat_base = ((uint32_t)(v->regs[11] & 0x03) << 15) |
                        ((uint32_t)(v->regs[5] & 0xFC) << 7);
    uint32_t ct = sat_base - 0x200;  // color table is 512 bytes before SAT
    uint32_t spg = (uint32_t)(v->regs[6] & 0x3F) << 11;
    int size = (v->regs[1] & 0x02) ? 16 : 8;
    int mag  = (v->regs[1] & 0x01) ? 2 : 1;
    int drawn = 0;
    for (int i = 0; i < 32 && drawn < 8; i++) {
        int y = vram_rd(v, sat_base + i * 4);
        if (y == 216) break;
        y = (y + 1) & 0xFF;
        if (line < y || line >= y + size * mag) continue;
        int x    = vram_rd(v, sat_base + i * 4 + 1);
        int pat  = vram_rd(v, sat_base + i * 4 + 2);
        int sy   = (line - y) / mag;
        uint8_t cattr = vram_rd(v, ct + i * 16 + sy);
        if (cattr & 0x40) x -= 32; // EC bit
        int clr  = cattr & 0x0F;
        bool cc  = (cattr & 0x20) != 0; // OR mode
        (void)cc;
        if (clr == 0 && !(cattr & 0x20)) { drawn++; continue; }
        uint32_t color = v->palette[clr];
        if (size == 16) pat &= 0xFC;
        for (int sx = 0; sx < size; sx++) {
            int bx = sx, by = sy;
            uint8_t bits;
            if (size == 16) {
                int quad = (bx >= 8 ? 1 : 0) + (by >= 8 ? 2 : 0);
                bits = vram_rd(v, spg + (pat + quad) * 8 + (by & 7));
                bx &= 7;
            } else {
                bits = vram_rd(v, spg + pat * 8 + by);
            }
            if (bits & (0x80 >> bx)) {
                for (int m = 0; m < mag; m++) {
                    int px = x + sx * mag + m;
                    if (px >= 0 && px < 256) buf[px] = color;
                }
            }
        }
        drawn++;
    }
}

// Render one scanline
static void vdp_render_line(MSX2_VDP* v, int line, uint32_t* fb) {
    int active = vdp_active_lines(v);
    int vscroll = v->regs[23];
    int top_blank = (active == 212) ? 0 : 10; // adjust for 192/212 modes
    int disp_line = line - (MSX_BORDER_V + top_blank);

    uint32_t border = v->palette[v->regs[7] & 0x0F];
    uint32_t* row = fb + (line * MSX_FULL_W);

    // Border or blank line?
    if (disp_line < 0 || disp_line >= active || !(v->regs[1] & 0x40)) {
        for (int x = 0; x < MSX_FULL_W; x++) row[x] = border;
        return;
    }

    // Left border
    for (int x = 0; x < MSX_BORDER_H; x++) row[x] = border;
    // Right border
    for (int x = MSX_BORDER_H + MSX_SCREEN_W; x < MSX_FULL_W; x++) row[x] = border;

    uint32_t* paper = row + MSX_BORDER_H;
    int render_line = (disp_line + vscroll) % active;
    int mode = vdp_screen_mode(v);

    switch (mode) {
    case 0:  vdp_render_t1(v, render_line, paper); break;
    case 1:  vdp_render_g1(v, render_line, paper); break;
    case 2:  // G2 (Screen 2)
    case 4:  // G3 (Screen 4)
        vdp_render_g2(v, render_line, paper); break;
    case 3:  vdp_render_mc(v, render_line, paper); break;
    case 5:  vdp_render_g4(v, render_line, paper); break;
    case 6:  vdp_render_g5(v, render_line, paper); break;
    case 7:  vdp_render_g6(v, render_line, paper); break;
    case 8:  vdp_render_g7(v, render_line, paper); break;
    case 10: // T2 (80 col) - render as T1 simplified
        vdp_render_t1(v, render_line, paper); break;
    default:
        for (int x = 0; x < MSX_SCREEN_W; x++) paper[x] = border;
        break;
    }

    // Sprites
    if (mode >= 1 && mode <= 3)
        vdp_render_sprites_m1(v, render_line, paper);
    else if (mode >= 4 && mode <= 8)
        vdp_render_sprites_m2(v, render_line, paper);
}

// =============================================================================
// V9938 VDP - Comandos
// =============================================================================

// Logical operation
static uint8_t vdp_log_op(int op, uint8_t src, uint8_t dst) {
    switch (op & 0x0F) {
    case 0x0: return src;              // IMP
    case 0x1: return src & dst;        // AND
    case 0x2: return src | dst;        // OR
    case 0x3: return src ^ dst;        // XOR
    case 0x4: return ~src & dst;       // NOT
    case 0x8: return src ? src : dst;  // TIMP
    case 0x9: return src ? (src & dst) : dst;
    case 0xA: return src ? (src | dst) : dst;
    case 0xB: return src ? (src ^ dst) : dst;
    case 0xC: return src ? (~src & dst) : dst;
    default:  return src;
    }
}

// Get pixel from VRAM (bitmap modes)
static uint8_t vdp_get_pixel(MSX2_VDP* v, int x, int y) {
    int mode = vdp_screen_mode(v);
    uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
    switch (mode) {
    case 5: { // G4: 4bpp, 256 wide
        uint32_t addr = base + (uint32_t)y * 128 + x / 2;
        uint8_t byte = vram_rd(v, addr);
        return (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
    }
    case 6: { // G5: 2bpp, 512 wide
        uint32_t addr = base + (uint32_t)y * 128 + x / 4;
        uint8_t byte = vram_rd(v, addr);
        int shift = (3 - (x & 3)) * 2;
        return (byte >> shift) & 0x03;
    }
    case 7: { // G6: 4bpp, 512 wide
        base = (uint32_t)(v->regs[2] & 0x20) << 11;
        uint32_t addr = base + (uint32_t)y * 256 + x / 2;
        uint8_t byte = vram_rd(v, addr);
        return (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
    }
    case 8: { // G7: 8bpp, 256 wide
        uint32_t addr = base + (uint32_t)y * 256 + x;
        return vram_rd(v, addr);
    }
    default: return 0;
    }
}

// Set pixel in VRAM (bitmap modes)
static void vdp_set_pixel(MSX2_VDP* v, int x, int y, uint8_t clr) {
    int mode = vdp_screen_mode(v);
    uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
    switch (mode) {
    case 5: { // G4
        uint32_t addr = base + (uint32_t)y * 128 + x / 2;
        uint8_t byte = vram_rd(v, addr);
        if (x & 1) byte = (byte & 0xF0) | (clr & 0x0F);
        else       byte = (byte & 0x0F) | ((clr & 0x0F) << 4);
        vram_wr(v, addr, byte);
        break;
    }
    case 6: { // G5
        uint32_t addr = base + (uint32_t)y * 128 + x / 4;
        uint8_t byte = vram_rd(v, addr);
        int shift = (3 - (x & 3)) * 2;
        uint8_t mask = 0x03 << shift;
        byte = (byte & ~mask) | ((clr & 0x03) << shift);
        vram_wr(v, addr, byte);
        break;
    }
    case 7: { // G6
        base = (uint32_t)(v->regs[2] & 0x20) << 11;
        uint32_t addr = base + (uint32_t)y * 256 + x / 2;
        uint8_t byte = vram_rd(v, addr);
        if (x & 1) byte = (byte & 0xF0) | (clr & 0x0F);
        else       byte = (byte & 0x0F) | ((clr & 0x0F) << 4);
        vram_wr(v, addr, byte);
        break;
    }
    case 8: { // G7
        uint32_t addr = base + (uint32_t)y * 256 + x;
        vram_wr(v, addr, clr);
        break;
    }
    default: break;
    }
}

// Execute VDP command instantly
static void vdp_exec_command(MSX2_VDP* v) {
    int cmd = v->regs[46] >> 4;
    int sx = v->regs[32] | ((v->regs[33] & 0x01) << 8);
    int sy = v->regs[34] | ((v->regs[35] & 0x03) << 8);
    int dx = v->regs[36] | ((v->regs[37] & 0x01) << 8);
    int dy = v->regs[38] | ((v->regs[39] & 0x03) << 8);
    int nx = v->regs[40] | ((v->regs[41] & 0x01) << 8);
    int ny = v->regs[42] | ((v->regs[43] & 0x03) << 8);
    int clr = v->regs[44];
    int arg = v->regs[45];
    int dix = (arg & 0x04) ? -1 : 1;
    int diy = (arg & 0x08) ? -1 : 1;
    int log = v->regs[46] & 0x0F;

    if (nx == 0) nx = 512;
    if (ny == 0) ny = 1024;

    v->cmd_busy = false;
    v->status[2] &= ~0x01; // clear TR
    v->status[2] &= ~0x80; // clear CE

    switch (cmd) {
    case 0x0: break; // STOP
    case 0x5: // PSET
        vdp_set_pixel(v, dx, dy, clr);
        break;
    case 0x4: // POINT
        v->status[7] = vdp_get_pixel(v, sx, sy);
        break;
    case 0x8: // LMMV (logical fill)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                int px = dx + x * dix, py = dy + y * diy;
                uint8_t dst = vdp_get_pixel(v, px & 0x1FF, py & 0x3FF);
                vdp_set_pixel(v, px & 0x1FF, py & 0x3FF, vdp_log_op(log, clr, dst));
            }
        break;
    case 0x9: // LMMM (logical move)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                int spx = sx + x * dix, spy = sy + y * diy;
                int dpx = dx + x * dix, dpy = dy + y * diy;
                uint8_t src = vdp_get_pixel(v, spx & 0x1FF, spy & 0x3FF);
                uint8_t dst = vdp_get_pixel(v, dpx & 0x1FF, dpy & 0x3FF);
                vdp_set_pixel(v, dpx & 0x1FF, dpy & 0x3FF, vdp_log_op(log, src, dst));
            }
        break;
    case 0xB: // LMMC (logical move CPU→VRAM) - set up for byte-at-a-time transfer
        v->cmd_op = cmd;
        v->cmd_dx = dx; v->cmd_dy = dy;
        v->cmd_nx = nx; v->cmd_ny = ny;
        v->cmd_px = 0;  v->cmd_py = 0;
        v->cmd_arg = arg;
        v->cmd_busy = true;
        v->status[2] |= 0x81; // CE + TR
        return;
    case 0xC: // HMMV (high-speed fill)
        for (int y = 0; y < ny; y++) {
            uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
            int py = (dy + y * diy) & 0x3FF;
            for (int x = 0; x < nx; x++) {
                int px = (dx + x * dix) & 0x1FF;
                uint32_t addr = base + (uint32_t)py * 128 + px / 2;
                if (vdp_screen_mode(v) == 8) // G7: byte per pixel
                    addr = base + (uint32_t)py * 256 + px;
                vram_wr(v, addr, clr);
            }
        }
        break;
    case 0xD: // HMMM (high-speed move)
        for (int y = 0; y < ny; y++) {
            uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
            int spy = (sy + y * diy) & 0x3FF;
            int dpy = (dy + y * diy) & 0x3FF;
            int bpl = (vdp_screen_mode(v) == 8) ? 256 : 128;
            for (int x = 0; x < nx; x++) {
                int spx = (sx + x * dix) & 0x1FF;
                int dpx = (dx + x * dix) & 0x1FF;
                uint32_t sa, da;
                if (bpl == 256) { sa = base + spy*256 + spx; da = base + dpy*256 + dpx; }
                else { sa = base + spy*128 + spx/2; da = base + dpy*128 + dpx/2; }
                vram_wr(v, da, vram_rd(v, sa));
            }
        }
        break;
    case 0xE: // YMMM (high-speed move Y-only)
        for (int y = 0; y < ny; y++) {
            int spy = (sy + y * diy) & 0x3FF;
            int dpy = (dy + y * diy) & 0x3FF;
            int bpl = (vdp_screen_mode(v) == 8) ? 256 : 128;
            uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
            for (int x = 0; x < bpl; x++)
                vram_wr(v, base + dpy * bpl + x, vram_rd(v, base + spy * bpl + x));
        }
        break;
    case 0xF: // HMMC (high-speed move CPU→VRAM) - byte transfer
        v->cmd_op = cmd;
        v->cmd_dx = dx; v->cmd_dy = dy;
        v->cmd_nx = nx; v->cmd_ny = ny;
        v->cmd_px = 0;  v->cmd_py = 0;
        v->cmd_arg = arg;
        v->cmd_busy = true;
        v->status[2] |= 0x81;
        return;
    case 0x7: // LINE
    {
        int maj = nx, min = ny;
        int cnt = 0;
        int px = dx, py = dy;
        int dmaj = (arg & 0x01) ? diy : dix;
        int dmin = (arg & 0x01) ? dix : diy;
        bool is_y_major = (arg & 0x01) != 0;
        for (int i = 0; i <= maj; i++) {
            uint8_t dst = vdp_get_pixel(v, px & 0x1FF, py & 0x3FF);
            vdp_set_pixel(v, px & 0x1FF, py & 0x3FF, vdp_log_op(log, clr, dst));
            cnt += min;
            if (cnt >= maj && maj > 0) {
                cnt -= maj;
                if (is_y_major) px += dmin; else py += dmin;
            }
            if (is_y_major) py += dmaj; else px += dmaj;
        }
        break;
    }
    case 0x6: // SRCH
    {
        int px = sx;
        bool eq = (arg & 0x02) == 0;
        for (int x = 0; x < 512; x++) {
            uint8_t p = vdp_get_pixel(v, px & 0x1FF, sy & 0x3FF);
            if ((eq && p == (clr & 0x0F)) || (!eq && p != (clr & 0x0F))) {
                v->status[2] |= 0x10; // BD
                v->status[8] = px & 0xFF;
                v->status[9] = (px >> 8) & 0x01;
                break;
            }
            px += dix;
        }
        break;
    }
    case 0xA: // LMCM (logical move VRAM→CPU)
        v->cmd_op = cmd;
        v->cmd_sx = sx; v->cmd_sy = sy;
        v->cmd_nx = nx; v->cmd_ny = ny;
        v->cmd_px = 0;  v->cmd_py = 0;
        v->cmd_arg = arg;
        v->cmd_busy = true;
        v->status[2] |= 0x81;
        // Pre-read first byte
        v->status[7] = vdp_get_pixel(v, sx, sy);
        return;
    default:
        break;
    }
}

// LMMC/HMMC byte transfer
static void vdp_cmd_write_byte(MSX2_VDP* v, uint8_t data) {
    if (!v->cmd_busy) return;
    int dix = (v->cmd_arg & 0x04) ? -1 : 1;
    int diy = (v->cmd_arg & 0x08) ? -1 : 1;

    if (v->cmd_op == 0x0B) { // LMMC
        int px = (v->cmd_dx + v->cmd_px * dix) & 0x1FF;
        int py = (v->cmd_dy + v->cmd_py * diy) & 0x3FF;
        int log = v->regs[46] & 0x0F;
        uint8_t dst = vdp_get_pixel(v, px, py);
        vdp_set_pixel(v, px, py, vdp_log_op(log, data, dst));
    } else if (v->cmd_op == 0x0F) { // HMMC
        int px = (v->cmd_dx + v->cmd_px * dix) & 0x1FF;
        int py = (v->cmd_dy + v->cmd_py * diy) & 0x3FF;
        uint32_t base = (uint32_t)(v->regs[2] & 0x60) << 10;
        int bpl = (vdp_screen_mode(v) == 8) ? 256 : 128;
        vram_wr(v, base + py * bpl + (bpl == 256 ? px : px / 2), data);
    }

    v->cmd_px++;
    if (v->cmd_px >= v->cmd_nx) {
        v->cmd_px = 0;
        v->cmd_py++;
        if (v->cmd_py >= v->cmd_ny) {
            v->cmd_busy = false;
            v->status[2] &= ~0x81;
        }
    }
}

// LMCM byte read
static uint8_t vdp_cmd_read_byte(MSX2_VDP* v) {
    if (!v->cmd_busy || v->cmd_op != 0x0A) return 0xFF;
    int dix = (v->cmd_arg & 0x04) ? -1 : 1;
    int diy = (v->cmd_arg & 0x08) ? -1 : 1;
    int px = (v->cmd_sx + v->cmd_px * dix) & 0x1FF;
    int py = (v->cmd_sy + v->cmd_py * diy) & 0x3FF;
    uint8_t val = vdp_get_pixel(v, px, py);
    v->cmd_px++;
    if (v->cmd_px >= v->cmd_nx) {
        v->cmd_px = 0;
        v->cmd_py++;
        if (v->cmd_py >= v->cmd_ny) {
            v->cmd_busy = false;
            v->status[2] &= ~0x81;
        }
    }
    // Pre-read next byte
    if (v->cmd_busy) {
        int npx = (v->cmd_sx + v->cmd_px * dix) & 0x1FF;
        int npy = (v->cmd_sy + v->cmd_py * diy) & 0x3FF;
        v->status[7] = vdp_get_pixel(v, npx, npy);
    }
    return val;
}

// =============================================================================
// V9938 VDP - Port I/O
// =============================================================================

static uint8_t vdp_port_read(MSX2_VDP* v, uint8_t port) {
    switch (port & 3) {
    case 0: { // Port 0x98: VRAM data read
        uint8_t val = v->read_buf;
        v->read_buf = vram_rd(v, v->vram_addr);
        v->vram_addr = (v->vram_addr + 1) & (VDP_VRAM_SIZE - 1);
        v->latch_flag = false;
        return val;
    }
    case 1: { // Port 0x99: Status register read
        uint8_t sr = v->regs[15] & 0x0F;
        if (sr >= VDP_NUM_STATUS) sr = 0;
        uint8_t val = v->status[sr];
        if (sr == 0) {
            v->status[0] &= 0x1F; // clear VBLANK int, overflow, collision flags
            v->irq_vblank = false;
        }
        if (sr == 1) {
            v->status[1] &= ~0x01; // clear HBLANK flag
            v->irq_hblank = false;
        }
        if (sr == 2 && v->cmd_busy && v->cmd_op == 0x0A) {
            val = v->status[2];
            v->status[7] = vdp_cmd_read_byte(v);
        }
        v->latch_flag = false;
        return val;
    }
    default:
        return 0xFF;
    }
}

static void vdp_port_write(MSX2_VDP* v, uint8_t port, uint8_t val) {
    switch (port & 3) {
    case 0: // Port 0x98: VRAM data write
        v->read_buf = val;
        vram_wr(v, v->vram_addr, val);
        v->vram_addr = (v->vram_addr + 1) & (VDP_VRAM_SIZE - 1);
        v->latch_flag = false;
        break;

    case 1: // Port 0x99: Register/address setup
        if (!v->latch_flag) {
            v->latch = val;
            v->latch_flag = true;
        } else {
            v->latch_flag = false;
            if (val & 0x80) {
                // Register write
                uint8_t reg = val & 0x3F;
                if (reg < VDP_NUM_REGS) {
                    v->regs[reg] = v->latch;
                    if (reg == 46 && v->latch >= 0x40) // Command register
                        vdp_exec_command(v);
                }
            } else {
                // VRAM address set
                v->vram_addr = ((uint32_t)(v->regs[14] & 0x07) << 14) |
                               ((uint32_t)(val & 0x3F) << 8) | v->latch;
                v->vram_write = (val & 0x40) != 0;
                if (!v->vram_write) {
                    v->read_buf = vram_rd(v, v->vram_addr);
                    v->vram_addr = (v->vram_addr + 1) & (VDP_VRAM_SIZE - 1);
                }
            }
        }
        break;

    case 2: // Port 0x9A: Palette write
        if (!v->pal_flag) {
            v->pal_latch = val;
            v->pal_flag = true;
        } else {
            v->pal_flag = false;
            uint8_t entry = v->regs[16] & 0x0F;
            uint8_t r = (v->pal_latch >> 4) & 0x07;
            uint8_t b = v->pal_latch & 0x07;
            uint8_t g = val & 0x07;
            v->pal_rgb[entry][0] = r;
            v->pal_rgb[entry][1] = g;
            v->pal_rgb[entry][2] = b;
            v->palette[entry] = rgb3_to_argb(r, g, b);
            v->regs[16] = (v->regs[16] + 1) & 0x0F;
        }
        break;

    case 3: // Port 0x9B: Indirect register access
    {
        uint8_t reg = v->regs[17] & 0x3F;
        if (reg < VDP_NUM_REGS && reg != 17) {
            v->regs[reg] = val;
            if (reg == 46 && val >= 0x40)
                vdp_exec_command(v);
        }
        if (!(v->regs[17] & 0x80))
            v->regs[17] = ((v->regs[17] & 0x80) | ((reg + 1) & 0x3F));
        break;
    }
    }
}

// =============================================================================
// AY-3-8910 PSG
// =============================================================================

static const uint16_t psg_vol_table[16] = {
    0, 73, 104, 147, 208, 295, 417, 590,
    836, 1181, 1671, 2362, 3340, 4723, 6678, 9441
};

static void psg_write_reg(MSX2_PSG* p, uint8_t reg, uint8_t val) {
    if (reg > 15) return;
    p->regs[reg] = val;
    switch (reg) {
    case 0: case 1:
        p->tone_period[0] = (p->regs[1] & 0x0F) << 8 | p->regs[0]; break;
    case 2: case 3:
        p->tone_period[1] = (p->regs[3] & 0x0F) << 8 | p->regs[2]; break;
    case 4: case 5:
        p->tone_period[2] = (p->regs[5] & 0x0F) << 8 | p->regs[4]; break;
    case 6:
        p->noise_period = val & 0x1F; break;
    case 11: case 12:
        p->env_period = p->regs[11] | (p->regs[12] << 8); break;
    case 13:
        p->env_shape = val & 0x0F;
        p->env_step = 0;
        p->env_cnt = 0;
        p->env_holding = false;
        p->env_attack = (val & 0x04) != 0;
        p->env_alternate = (val & 0x02) != 0;
        break;
    default: break;
    }
}

static void psg_advance(MSX2_PSG* p, int cpu_cycles) {
    p->cycle_accum += cpu_cycles;
    while (p->cycle_accum >= 32) {
        p->cycle_accum -= 32;

        // Tone generators
        for (int ch = 0; ch < 3; ch++) {
            if (p->tone_period[ch] == 0) continue;
            p->tone_cnt[ch]++;
            if (p->tone_cnt[ch] >= p->tone_period[ch]) {
                p->tone_cnt[ch] = 0;
                p->tone_out[ch] ^= 1;
            }
        }

        // Noise
        uint8_t np = p->noise_period ? p->noise_period : 1;
        p->noise_cnt++;
        if (p->noise_cnt >= np) {
            p->noise_cnt = 0;
            // 17-bit LFSR: bit 0 XOR bit 3
            p->noise_shift = (p->noise_shift >> 1) |
                             (((p->noise_shift ^ (p->noise_shift >> 3)) & 1) << 16);
            p->noise_out = p->noise_shift & 1;
        }

        // Envelope
        if (!p->env_holding) {
            uint16_t ep = p->env_period ? p->env_period : 1;
            p->env_cnt++;
            if (p->env_cnt >= ep) {
                p->env_cnt = 0;
                p->env_step++;
                if (p->env_step >= 32) {
                    if (p->env_shape < 0x08 || !(p->env_shape & 0x01)) {
                        // Non-continuing: hold at 0
                        p->env_holding = true;
                        p->env_step = 31;
                    } else if (p->env_alternate) {
                        p->env_attack = !p->env_attack;
                        p->env_step = 0;
                    } else {
                        p->env_step = 0;
                    }
                }
            }
        }
    }
}

static int psg_env_volume(MSX2_PSG* p) {
    int step = p->env_step;
    if (step >= 32) step = 31;
    int half = step < 16 ? step : 31 - step;
    return p->env_attack ? half : (15 - half);
}

static float psg_sample(MSX2_PSG* p) {
    uint8_t mixer = p->regs[7];
    float out = 0.0f;
    for (int ch = 0; ch < 3; ch++) {
        bool tone_en = !(mixer & (1 << ch));
        bool noise_en = !(mixer & (8 << ch));
        bool tone_val = p->tone_out[ch] || !tone_en;
        bool noise_val = p->noise_out || !noise_en;
        if (tone_val && noise_val) {
            int vol_reg = p->regs[8 + ch] & 0x1F;
            int vol;
            if (vol_reg & 0x10)
                vol = psg_env_volume(p);
            else
                vol = vol_reg & 0x0F;
            out += (float)psg_vol_table[vol] / 9441.0f;
        }
    }
    return out / 3.0f;
}

// =============================================================================
// Slot system - recalculate page pointers
// =============================================================================

static void mem_update_pages(MSX2* m) {
    for (int page = 0; page < 4; page++) {
        int ps = (m->mem.primary_sel >> (page * 2)) & 3;
        int ss = 0;
        if (m->mem.expanded[ps])
            ss = (m->mem.secondary_sel[ps] >> (page * 2)) & 3;

        m->mem.wr[page] = NULL; // default: read-only

        if (ps == 0 && ss == 0) {
            // Main BIOS ROM (pages 0-1)
            if (page < 2)
                m->mem.rd[page] = m->mem.bios + page * 16384;
            else
                m->mem.rd[page] = m->mem.ram; // fallback
        } else if (ps == 0 && ss == 1) {
            // Sub-ROM (page 0 only)
            if (page == 0)
                m->mem.rd[page] = m->mem.subrom;
            else
                m->mem.rd[page] = m->mem.ram;
        } else if (ps == 1) {
            // Cartridge slot
            if (m->mem.cart_size > 0) {
                if (m->mem.cart_mapper == MAPPER_NONE) {
                    // Plain ROM
                    uint32_t offset = 0;
                    if (m->mem.cart_size <= 16384) {
                        // 16K ROM at page 1
                        if (page == 1) { m->mem.rd[page] = m->mem.cart_data; }
                        else m->mem.rd[page] = m->mem.ram;
                    } else if (m->mem.cart_size <= 32768) {
                        // 32K ROM at pages 1-2
                        if (page == 1) m->mem.rd[page] = m->mem.cart_data;
                        else if (page == 2) m->mem.rd[page] = m->mem.cart_data + 16384;
                        else m->mem.rd[page] = m->mem.ram;
                    } else {
                        // 48K/64K
                        offset = page * 16384;
                        if (offset < m->mem.cart_size)
                            m->mem.rd[page] = m->mem.cart_data + offset;
                        else
                            m->mem.rd[page] = m->mem.ram;
                    }
                } else {
                    // MegaROM - calculated per 8K bank in mem_read
                    m->mem.rd[page] = NULL; // handled specially
                }
            } else {
                m->mem.rd[page] = m->mem.ram;
            }
        } else if (ps == 2) {
            // Cartridge slot 2 (empty for now)
            m->mem.rd[page] = m->mem.ram;
        } else if (ps == 3) {
            if (m->mem.expanded[3]) {
                if (ss == 0) {
                    // RAM (memory mapped)
                    int ram_page = m->mem.mapper_reg[page] % MSX_RAM_PAGES;
                    m->mem.rd[page] = m->mem.ram + ram_page * 16384;
                    m->mem.wr[page] = m->mem.rd[page];
                } else if (ss == 1) {
                    // Disk ROM (page 1 only) + Sub-ROM (page 0)
                    if (page == 0)
                        m->mem.rd[page] = m->mem.subrom;
                    else if (page == 1 && m->mem.has_diskrom)
                        m->mem.rd[page] = m->mem.diskrom;
                    else {
                        int ram_page = m->mem.mapper_reg[page] % MSX_RAM_PAGES;
                        m->mem.rd[page] = m->mem.ram + ram_page * 16384;
                        m->mem.wr[page] = m->mem.rd[page];
                    }
                } else {
                    // Sub-slots 2,3: RAM
                    int ram_page = m->mem.mapper_reg[page] % MSX_RAM_PAGES;
                    m->mem.rd[page] = m->mem.ram + ram_page * 16384;
                    m->mem.wr[page] = m->mem.rd[page];
                }
            } else {
                // Non-expanded slot 3: RAM
                int ram_page = m->mem.mapper_reg[page] % MSX_RAM_PAGES;
                m->mem.rd[page] = m->mem.ram + ram_page * 16384;
                m->mem.wr[page] = m->mem.rd[page];
            }
        }
    }
}

// =============================================================================
// MegaROM - read 8KB bank
// =============================================================================

static uint8_t megarom_read(MSX2* m, uint16_t addr) {
    int bank_idx;
    switch (m->mem.cart_mapper) {
    case MAPPER_ASCII8:
    case MAPPER_KONAMI_SCC:
        bank_idx = (addr >> 13) - 2; // 0x4000→0, 0x6000→1, 0x8000→2, 0xA000→3
        break;
    case MAPPER_KONAMI:
        bank_idx = (addr >> 13) - 2;
        break;
    case MAPPER_ASCII16: {
        int page16 = (addr >> 14) & 1; // 0x4000 or 0x8000
        uint32_t base = (uint32_t)m->mem.cart_bank[page16] * 16384;
        uint32_t offset = addr & 0x3FFF;
        if (base + offset < m->mem.cart_size)
            return m->mem.cart_data[base + offset];
        return 0xFF;
    }
    default:
        return 0xFF;
    }
    uint32_t base = (uint32_t)m->mem.cart_bank[bank_idx] * 8192;
    uint32_t offset = addr & 0x1FFF;
    if (base + offset < m->mem.cart_size)
        return m->mem.cart_data[base + offset];
    return 0xFF;
}

// MegaROM - bank switch write
static void megarom_write(MSX2* m, uint16_t addr, uint8_t val) {
    switch (m->mem.cart_mapper) {
    case MAPPER_ASCII8:
        if (addr >= 0x6000 && addr < 0x6800)      m->mem.cart_bank[0] = val;
        else if (addr >= 0x6800 && addr < 0x7000)  m->mem.cart_bank[1] = val;
        else if (addr >= 0x7000 && addr < 0x7800)  m->mem.cart_bank[2] = val;
        else if (addr >= 0x7800 && addr < 0x8000)  m->mem.cart_bank[3] = val;
        break;
    case MAPPER_ASCII16:
        if (addr >= 0x6000 && addr < 0x6800)       m->mem.cart_bank[0] = val;
        else if (addr >= 0x7000 && addr < 0x7800)  m->mem.cart_bank[1] = val;
        break;
    case MAPPER_KONAMI:
        if (addr >= 0x6000 && addr < 0x8000)       m->mem.cart_bank[1] = val;
        else if (addr >= 0x8000 && addr < 0xA000)  m->mem.cart_bank[2] = val;
        else if (addr >= 0xA000 && addr < 0xC000)  m->mem.cart_bank[3] = val;
        break;
    case MAPPER_KONAMI_SCC:
        if (addr >= 0x5000 && addr < 0x5800)       m->mem.cart_bank[0] = val;
        else if (addr >= 0x7000 && addr < 0x7800)  m->mem.cart_bank[1] = val;
        else if (addr >= 0x9000 && addr < 0x9800)  m->mem.cart_bank[2] = val;
        else if (addr >= 0xB000 && addr < 0xB800)  m->mem.cart_bank[3] = val;
        break;
    default: break;
    }
}

// =============================================================================
// CPU memory callbacks
// =============================================================================

static uint8_t mem_read(void* userdata, uint16_t addr) {
    MSX2* m = (MSX2*)userdata;

    // Sub-slot register: read returns complement of written value
    if (addr == 0xFFFF) {
        int ps = (m->mem.primary_sel >> 6) & 3;
        if (m->mem.expanded[ps])
            return ~m->mem.secondary_sel[ps];
    }

    int page = addr >> 14;
    int ps = (m->mem.primary_sel >> (page * 2)) & 3;

    // MegaROM special handling
    if (ps == 1 && m->mem.cart_mapper != MAPPER_NONE &&
        m->mem.cart_size > 0 && addr >= 0x4000 && addr < 0xC000) {
        return megarom_read(m, addr);
    }

    if (m->mem.rd[page])
        return m->mem.rd[page][addr & 0x3FFF];
    return 0xFF;
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    MSX2* m = (MSX2*)userdata;

    // Sub-slot register
    if (addr == 0xFFFF) {
        int ps = (m->mem.primary_sel >> 6) & 3;
        if (m->mem.expanded[ps]) {
            m->mem.secondary_sel[ps] = val;
            mem_update_pages(m);
            return;
        }
    }

    int page = addr >> 14;
    int ps = (m->mem.primary_sel >> (page * 2)) & 3;

    // MegaROM bank switching
    if (ps == 1 && m->mem.cart_mapper != MAPPER_NONE &&
        m->mem.cart_size > 0 && addr >= 0x4000 && addr < 0xC000) {
        megarom_write(m, addr, val);
        return;
    }

    if (m->mem.wr[page])
        m->mem.wr[page][addr & 0x3FFF] = val;
}

// =============================================================================
// I/O port callbacks
// =============================================================================

static uint8_t rtc_read(MSX2_RTC* r, uint8_t addr_nibble);

static uint8_t port_in(z80* z, uint16_t port) {
    MSX2* m = (MSX2*)z->userdata;
    uint8_t p = port & 0xFF;

    switch (p) {
    // VDP
    case 0x98: case 0x99: case 0x9A: case 0x9B:
        return vdp_port_read(&m->vdp, p - 0x98);

    // PSG
    case 0xA2:
        if (m->psg.addr == 14) {
            // Port A: joystick + cassette input
            // Bits 5-0: joystick (active low), bit 6: cassette, bit 7: unused
            return m->joy1;
        }
        if (m->psg.addr <= 15)
            return m->psg.regs[m->psg.addr];
        return 0xFF;

    // PPI 8255
    case 0xA8: return m->ppi.slot_sel;
    case 0xA9: {
        // Keyboard matrix read
        int row = m->ppi.port_c & 0x0F;
        if (row < 11) return m->keyboard[row];
        return 0xFF;
    }
    case 0xAA: return m->ppi.port_c;
    case 0xAB: return 0xFF;

    // RTC RP5C01
    case 0xB5:
        return rtc_read(&m->rtc, m->rtc.reg_addr);

    // Memory mapper
    case 0xFC: return m->mem.mapper_reg[0];
    case 0xFD: return m->mem.mapper_reg[1];
    case 0xFE: return m->mem.mapper_reg[2];
    case 0xFF: return m->mem.mapper_reg[3];

    default: return 0xFF;
    }
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    MSX2* m = (MSX2*)z->userdata;
    uint8_t p = port & 0xFF;

    switch (p) {
    // VDP
    case 0x98: case 0x99: case 0x9A: case 0x9B:
        if (m->vdp.cmd_busy && (m->vdp.cmd_op == 0x0B || m->vdp.cmd_op == 0x0F) && p == 0x98) {
            // During LMMC/HMMC, port 0 writes feed the command engine only
            vdp_cmd_write_byte(&m->vdp, val);
        } else {
            vdp_port_write(&m->vdp, p - 0x98, val);
        }
        break;

    // PSG
    case 0xA0: m->psg.addr = val & 0x0F; break;
    case 0xA1: psg_write_reg(&m->psg, m->psg.addr, val); break;

    // PPI 8255
    case 0xA8:
        m->ppi.slot_sel = val;
        m->mem.primary_sel = val;
        mem_update_pages(m);
        break;
    case 0xAA:
        m->ppi.port_c = val;
        m->ppi.key_row = val & 0x0F;
        break;
    case 0xAB:
        if (val & 0x80) {
            // Mode set (ignore, we don't fully emulate 8255 modes)
        } else {
            // Bit set/reset on port C
            int bit = (val >> 1) & 0x07;
            if (val & 0x01)
                m->ppi.port_c |= (1 << bit);
            else
                m->ppi.port_c &= ~(1 << bit);
            m->ppi.key_row = m->ppi.port_c & 0x0F;
        }
        break;

    // RTC RP5C01
    case 0xB4:
        m->rtc.reg_addr = val & 0x0F; break;
    case 0xB5: {
        int reg = m->rtc.reg_addr;
        if (reg == 13) {
            m->rtc.mode = val & 0x0F;
        } else {
            int block = m->rtc.mode & 0x03;
            if (reg < 13 && block >= 2)
                m->rtc.ram[block][reg] = val & 0x0F;
        }
        break;
    }

    // Memory mapper
    case 0xFC:
        m->mem.mapper_reg[0] = val;
        mem_update_pages(m);
        break;
    case 0xFD:
        m->mem.mapper_reg[1] = val;
        mem_update_pages(m);
        break;
    case 0xFE:
        m->mem.mapper_reg[2] = val;
        mem_update_pages(m);
        break;
    case 0xFF:
        m->mem.mapper_reg[3] = val;
        mem_update_pages(m);
        break;

    // Printer (ignore)
    case 0x90: case 0x91: break;

    default: break;
    }
}

// =============================================================================
// CAS tape - BIOS level trapping
// =============================================================================

static const uint8_t cas_header[] = {0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74};

// Find next header in CAS file (aligned to 8 bytes)
static bool cas_find_header(MSX2_CAS* c) {
    while (c->pos + 8 <= c->size) {
        // Align to 8 bytes
        if (c->pos & 7) { c->pos = (c->pos + 7) & ~7; continue; }
        if (memcmp(c->data + c->pos, cas_header, 8) == 0) {
            c->pos += 8;
            return true;
        }
        c->pos += 8;
    }
    return false;
}

// Read one byte from CAS file
static bool cas_read_byte(MSX2_CAS* c, uint8_t* out) {
    if (c->pos >= c->size) return false;
    // Check if we're at a header (skip it)
    if (c->pos + 8 <= c->size && (c->pos & 7) == 0 &&
        memcmp(c->data + c->pos, cas_header, 8) == 0) {
        return false; // end of block
    }
    *out = c->data[c->pos++];
    return true;
}

// BIOS TAPION trap: find header and set carry flag
static void cas_trap_tapion(MSX2* m) {
    if (!m->cas.loaded) {
        m->cpu.f |= 0x01; // Set carry = error
        m->cpu.a = 0;
        return;
    }
    if (cas_find_header(&m->cas)) {
        m->cpu.f &= ~0x01; // Clear carry = OK
        m->cpu.a = 0;
    } else {
        m->cpu.f |= 0x01;
        m->cpu.a = 0;
    }
}

// BIOS TAPIN trap: read one byte
static void cas_trap_tapin(MSX2* m) {
    uint8_t byte;
    if (m->cas.loaded && cas_read_byte(&m->cas, &byte)) {
        m->cpu.a = byte;
        m->cpu.f &= ~0x01; // Clear carry
    } else {
        m->cpu.f |= 0x01; // Set carry = error/end
    }
}

// =============================================================================
// DSK disk - BIOS level trapping
// =============================================================================

// DSKIO trap: read/write sectors
static void dsk_trap_dskio(MSX2* m) {
    if (!m->dsk.loaded) {
        m->cpu.a = 2; // not ready
        m->cpu.f |= 0x01;
        return;
    }
    bool write = (m->cpu.f & 0x01) != 0;
    int nsectors = m->cpu.b;
    int sector = m->cpu.e | (m->cpu.d << 8);
    uint16_t buf = m->cpu.hl;

    for (int i = 0; i < nsectors; i++) {
        uint32_t offset = (uint32_t)(sector + i) * 512;
        if (offset + 512 > m->dsk.size) {
            m->cpu.a = 6; // seek error
            m->cpu.b = nsectors - i;
            m->cpu.f |= 0x01;
            return;
        }
        if (write) {
            for (int j = 0; j < 512; j++)
                m->dsk.data[offset + j] = mem_read(m, (uint16_t)(buf + j));
        } else {
            for (int j = 0; j < 512; j++)
                mem_write(m, (uint16_t)(buf + j), m->dsk.data[offset + j]);
        }
        buf += 512;
    }
    m->cpu.a = 0;
    m->cpu.b = 0;
    m->cpu.f &= ~0x01;
}

// DSKCHG trap: disk changed?
static void dsk_trap_dskchg(MSX2* m) {
    m->cpu.b = 0; // not changed
    m->cpu.f &= ~0x01;
}

// GETDPB trap
static void dsk_trap_getdpb(MSX2* m) {
    // Write DPB at HL+1
    uint16_t addr = m->cpu.hl + 1;
    // Standard 720KB DPB
    static const uint8_t dpb[] = {
        0xF9,       // media descriptor
        0x00, 0x02, // sector size (512)
        0x0F,       // directory mask
        0x04,       // directory shift
        0x01,       // cluster mask
        0x02,       // cluster shift
        0x01, 0x00, // first FAT sector
        0x02,       // number of FATs
        0x70,       // directory entries
        0x0E, 0x00, // first data sector
        0x5A, 0x02, // number of clusters + 1
        0x03,       // sectors per FAT
        0x07, 0x00  // first directory sector
    };
    for (int i = 0; i < (int)sizeof(dpb); i++)
        mem_write(m, addr + i, dpb[i]);
    m->cpu.f &= ~0x01;
}

// =============================================================================
// RTC RP5C01 - read current time
// =============================================================================

static uint8_t rtc_read(MSX2_RTC* r, uint8_t addr_nibble) {
    int block = r->mode & 0x03;
    int reg = addr_nibble & 0x0F;
    if (reg >= 13) {
        if (reg == 13) return r->mode;
        return 0x0F;
    }
    if (block == 0) {
        // Time registers
        time_t now = time(NULL);
        struct tm* t = localtime(&now);
        switch (reg) {
        case 0: return t->tm_sec % 10;
        case 1: return t->tm_sec / 10;
        case 2: return t->tm_min % 10;
        case 3: return t->tm_min / 10;
        case 4: return t->tm_hour % 10;
        case 5: return t->tm_hour / 10;
        case 6: return t->tm_wday;
        case 7: return t->tm_mday % 10;
        case 8: return t->tm_mday / 10;
        case 9: return (t->tm_mon + 1) % 10;
        case 10: return (t->tm_mon + 1) / 10;
        case 11: return (t->tm_year % 100) % 10;
        case 12: return ((t->tm_year % 100) / 10) & 0x0F;
        default: return 0;
        }
    }
    return r->ram[block][reg];
}

// =============================================================================
// Inicialización
// =============================================================================

void msx2_init(MSX2* m) {
    memset(m, 0, sizeof(MSX2));

    // Keyboard: all keys released (0xFF)
    for (int i = 0; i < 11; i++) m->keyboard[i] = 0xFF;
    m->joy1 = 0x3F; // all buttons released (active low)

    // Slot configuration: slot 3 expanded (RAM + Disk ROM)
    m->mem.expanded[0] = true;  // Slot 0: BIOS (0-0) + Sub-ROM (0-1)
    m->mem.expanded[3] = true;  // Slot 3: RAM (3-0) + Disk ROM (3-1)

    // Memory mapper: initial pages
    m->mem.mapper_reg[0] = 3; // page 0 → RAM page 3
    m->mem.mapper_reg[1] = 2; // page 1 → RAM page 2
    m->mem.mapper_reg[2] = 1; // page 2 → RAM page 1
    m->mem.mapper_reg[3] = 0; // page 3 → RAM page 0

    // Initial slot selection: BIOS in pages 0-1, RAM in pages 2-3
    m->mem.primary_sel = 0xF0; // page3=slot3, page2=slot3, page1=slot0, page0=slot0
    m->ppi.slot_sel = m->mem.primary_sel;

    // Initialize VDP palette
    for (int i = 0; i < 16; i++) {
        m->vdp.pal_rgb[i][0] = v9938_default_pal[i][0];
        m->vdp.pal_rgb[i][1] = v9938_default_pal[i][1];
        m->vdp.pal_rgb[i][2] = v9938_default_pal[i][2];
        m->vdp.palette[i] = rgb3_to_argb(v9938_default_pal[i][0],
                                          v9938_default_pal[i][1],
                                          v9938_default_pal[i][2]);
    }

    // VDP default registers
    m->vdp.regs[9] = 0x02; // PAL mode

    // PSG noise LFSR seed
    m->psg.noise_shift = 0x12345;

    // CPU init
    z80_init(&m->cpu);
    m->cpu.userdata   = m;
    m->cpu.read_byte  = mem_read;
    m->cpu.write_byte = mem_write;
    m->cpu.port_in    = port_in;
    m->cpu.port_out   = port_out;

    mem_update_pages(m);
    z80_reset(&m->cpu);

    // SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    m->window = SDL_CreateWindow("MSX2",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 MSX_FULL_W * MSX_SCALE, MSX_FULL_H * MSX_SCALE,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    m->renderer = SDL_CreateRenderer(m->window, -1,
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    m->texture = SDL_CreateTexture(m->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, MSX_FULL_W, MSX_FULL_H);
    SDL_RenderSetLogicalSize(m->renderer, MSX_FULL_W * MSX_SCALE, MSX_FULL_H * MSX_SCALE);

    SDL_AudioSpec wanted, have;
    SDL_zero(wanted);
    wanted.freq = MSX_AUDIO_RATE;
    wanted.format = AUDIO_F32;
    wanted.channels = 1;
    wanted.samples = 1024;
    m->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &have, 0);
    if (m->audio_dev > 0) SDL_PauseAudioDevice(m->audio_dev, 0);
}

void msx2_destroy(MSX2* m) {
    free(m->cas.data);
    free(m->dsk.data);
    SDL_DestroyTexture(m->texture);
    SDL_DestroyRenderer(m->renderer);
    SDL_DestroyWindow(m->window);
    SDL_CloseAudioDevice(m->audio_dev);
    SDL_Quit();
}

// =============================================================================
// ROM / BIOS loading
// =============================================================================

static int load_file(const char* path, uint8_t* buf, size_t max) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > (long)max) sz = (long)max;
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return (int)rd;
}

int msx2_load_bios(MSX2* m, const char* dir) {
    char path[512];
    int loaded = 0;

    // Try various common BIOS filenames
    static const char* bios_names[] = {"MSX2.ROM", "msx2.rom", "MSX2BIOS.ROM", "msx2bios.rom", NULL};
    static const char* sub_names[]  = {"MSX2EXT.ROM", "msx2ext.rom", "MSX2SUB.ROM", "msx2sub.rom", NULL};
    static const char* disk_names[] = {"DISK.ROM", "disk.rom", "DISKROM.ROM", "diskrom.rom", NULL};

    for (int i = 0; bios_names[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, bios_names[i]);
        if (load_file(path, m->mem.bios, 32768) > 0) { loaded++; break; }
    }
    for (int i = 0; sub_names[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, sub_names[i]);
        if (load_file(path, m->mem.subrom, 16384) > 0) { loaded++; break; }
    }
    for (int i = 0; disk_names[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, disk_names[i]);
        if (load_file(path, m->mem.diskrom, 16384) > 0) {
            m->mem.has_diskrom = true;
            loaded++;
            break;
        }
    }

    if (loaded < 2) {
        fprintf(stderr, "Necesita al menos MSX2.ROM y MSX2EXT.ROM en %s\n", dir);
        return -1;
    }

    mem_update_pages(m);
    z80_reset(&m->cpu);
    return 0;
}

// =============================================================================
// ROM cartridge loading
// =============================================================================

static MSX2MapperType detect_mapper(const uint8_t* data, uint32_t size) {
    if (size <= 65536) return MAPPER_NONE;

    // Simple heuristic: count writes to mapper-specific addresses
    int ascii8 = 0, ascii16 = 0, konami = 0, konami_scc = 0;

    for (uint32_t i = 0; i + 2 < size; i++) {
        if (data[i] == 0x32) { // LD (nn),A
            uint16_t addr = data[i+1] | (data[i+2] << 8);
            if (addr >= 0x6000 && addr < 0x6800) ascii8++;
            if (addr >= 0x6800 && addr < 0x7000) ascii8++;
            if (addr >= 0x7000 && addr < 0x7800) { ascii8++; ascii16++; }
            if (addr >= 0x7800 && addr < 0x8000) ascii8++;
            if (addr >= 0x5000 && addr < 0x5800) konami_scc++;
            if (addr >= 0x7000 && addr < 0x7800) konami_scc++;
            if (addr >= 0x9000 && addr < 0x9800) konami_scc++;
            if (addr >= 0xB000 && addr < 0xB800) konami_scc++;
            if (addr >= 0x6000 && addr < 0x8000) konami++;
            if (addr >= 0x8000 && addr < 0xA000) konami++;
            if (addr >= 0xA000 && addr < 0xC000) konami++;
        }
    }

    if (konami_scc > ascii8 && konami_scc > konami) return MAPPER_KONAMI_SCC;
    if (konami > ascii8 && konami > ascii16) return MAPPER_KONAMI;
    if (ascii16 > ascii8) return MAPPER_ASCII16;
    return MAPPER_ASCII8;
}

int msx2_load_rom(MSX2* m, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > MSX_ROM_CART_MAX) sz = MSX_ROM_CART_MAX;
    size_t rd = fread(m->mem.cart_data, 1, (size_t)sz, f);
    fclose(f);
    m->mem.cart_size = (uint32_t)rd;

    m->mem.cart_mapper = detect_mapper(m->mem.cart_data, (uint32_t)rd);

    if (m->mem.cart_mapper != MAPPER_NONE) {
        // Initial bank mapping
        m->mem.cart_bank[0] = 0;
        m->mem.cart_bank[1] = 1;
        m->mem.cart_bank[2] = 2;
        m->mem.cart_bank[3] = 3;
    }

    printf("[ROM] Cargado %s (%u KB, mapper=%d)\n", filename,
           (unsigned)(rd / 1024), m->mem.cart_mapper);

    // Update slot config: cartridge in slot 1
    mem_update_pages(m);
    return 0;
}

// =============================================================================
// CAS loading
// =============================================================================

int msx2_load_cas(MSX2* m, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(m->cas.data);
    m->cas.data = (uint8_t*)malloc((size_t)sz);
    if (!m->cas.data) { fclose(f); return -1; }
    m->cas.size = (uint32_t)fread(m->cas.data, 1, (size_t)sz, f);
    fclose(f);
    m->cas.pos = 0;
    m->cas.loaded = true;
    printf("[CAS] Cargado %s (%u bytes)\n", filename, m->cas.size);
    return 0;
}

// =============================================================================
// DSK loading
// =============================================================================

int msx2_load_dsk(MSX2* m, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(m->dsk.data);
    m->dsk.data = (uint8_t*)malloc((size_t)sz);
    if (!m->dsk.data) { fclose(f); return -1; }
    m->dsk.size = (uint32_t)fread(m->dsk.data, 1, (size_t)sz, f);
    fclose(f);

    if (sz >= 737280) { m->dsk.sides = 2; m->dsk.tracks = 80; m->dsk.sectors = 9; }
    else              { m->dsk.sides = 1; m->dsk.tracks = 80; m->dsk.sectors = 9; }
    m->dsk.loaded = true;
    printf("[DSK] Cargado %s (%u bytes, %d lados)\n", filename, m->dsk.size, m->dsk.sides);
    return 0;
}

// =============================================================================
// Keyboard mapping (SDL scancode → MSX matrix)
// =============================================================================

typedef struct { uint8_t row; uint8_t bit; } KeyMap;

static const struct { SDL_Scancode sc; uint8_t row; uint8_t bit; } keymap[] = {
    // Row 0: 7=; 6=] 5=[ 4=\ 3== 2=- 1=9 0=8
    {SDL_SCANCODE_SEMICOLON,  0, 7}, {SDL_SCANCODE_RIGHTBRACKET, 0, 6},
    {SDL_SCANCODE_LEFTBRACKET, 0, 5}, {SDL_SCANCODE_BACKSLASH,  0, 4},
    {SDL_SCANCODE_EQUALS,    0, 3}, {SDL_SCANCODE_MINUS,       0, 2},
    {SDL_SCANCODE_9,         0, 1}, {SDL_SCANCODE_8,           0, 0},
    // Row 1: 7=B 6=A 5=_ 4=/ 3=. 2=, 1=` 0='
    {SDL_SCANCODE_B,         1, 7}, {SDL_SCANCODE_A,           1, 6},
    {SDL_SCANCODE_SLASH,     1, 4}, {SDL_SCANCODE_PERIOD,      1, 3},
    {SDL_SCANCODE_COMMA,     1, 2}, {SDL_SCANCODE_GRAVE,       1, 1},
    {SDL_SCANCODE_APOSTROPHE, 1, 0},
    // Row 2: 7=J 6=I 5=H 4=G 3=F 2=E 1=D 0=C
    {SDL_SCANCODE_J, 2, 7}, {SDL_SCANCODE_I, 2, 6}, {SDL_SCANCODE_H, 2, 5},
    {SDL_SCANCODE_G, 2, 4}, {SDL_SCANCODE_F, 2, 3}, {SDL_SCANCODE_E, 2, 2},
    {SDL_SCANCODE_D, 2, 1}, {SDL_SCANCODE_C, 2, 0},
    // Row 3: 7=R 6=Q 5=P 4=O 3=N 2=M 1=L 0=K
    {SDL_SCANCODE_R, 3, 7}, {SDL_SCANCODE_Q, 3, 6}, {SDL_SCANCODE_P, 3, 5},
    {SDL_SCANCODE_O, 3, 4}, {SDL_SCANCODE_N, 3, 3}, {SDL_SCANCODE_M, 3, 2},
    {SDL_SCANCODE_L, 3, 1}, {SDL_SCANCODE_K, 3, 0},
    // Row 4: 7=Z 6=Y 5=X 4=W 3=V 2=U 1=T 0=S
    {SDL_SCANCODE_Z, 4, 7}, {SDL_SCANCODE_Y, 4, 6}, {SDL_SCANCODE_X, 4, 5},
    {SDL_SCANCODE_W, 4, 4}, {SDL_SCANCODE_V, 4, 3}, {SDL_SCANCODE_U, 4, 2},
    {SDL_SCANCODE_T, 4, 1}, {SDL_SCANCODE_S, 4, 0},
    // Row 5: 7=F3 6=F2 5=F1 4=CODE 3=CAPS 2=GRAPH 1=CTRL 0=SHIFT
    {SDL_SCANCODE_F3,      5, 7}, {SDL_SCANCODE_F2,       5, 6},
    {SDL_SCANCODE_F1,      5, 5}, {SDL_SCANCODE_RALT,     5, 4},
    {SDL_SCANCODE_CAPSLOCK, 5, 3}, {SDL_SCANCODE_LALT,    5, 2},
    {SDL_SCANCODE_LCTRL,   5, 1}, {SDL_SCANCODE_LSHIFT,  5, 0},
    // Row 6: 7=RET 6=SEL 5=BS 4=STOP 3=TAB 2=ESC 1=F5 0=F4
    {SDL_SCANCODE_RETURN,  6, 7}, {SDL_SCANCODE_F6,       6, 6},
    {SDL_SCANCODE_BACKSPACE, 6, 5}, {SDL_SCANCODE_F8,     6, 4},
    {SDL_SCANCODE_TAB,     6, 3}, {SDL_SCANCODE_ESCAPE,  6, 2},
    {SDL_SCANCODE_F5,      6, 1}, {SDL_SCANCODE_F4,      6, 0},
    // Row 7: 7=RIGHT 6=DOWN 5=UP 4=LEFT 3=DEL 2=INS 1=HOME 0=SPACE
    {SDL_SCANCODE_RIGHT,   7, 7}, {SDL_SCANCODE_DOWN,    7, 6},
    {SDL_SCANCODE_UP,      7, 5}, {SDL_SCANCODE_LEFT,    7, 4},
    {SDL_SCANCODE_DELETE,  7, 3}, {SDL_SCANCODE_INSERT,  7, 2},
    {SDL_SCANCODE_HOME,    7, 1}, {SDL_SCANCODE_SPACE,   7, 0},
    // Row 8: numeric keys
    {SDL_SCANCODE_0, 8, 7}, {SDL_SCANCODE_1, 8, 6}, {SDL_SCANCODE_2, 8, 5},
    {SDL_SCANCODE_3, 8, 4}, {SDL_SCANCODE_4, 8, 3}, {SDL_SCANCODE_5, 8, 2},
    {SDL_SCANCODE_6, 8, 1}, {SDL_SCANCODE_7, 8, 0},
    // RSHIFT
    {SDL_SCANCODE_RSHIFT, 5, 0},
    {SDL_SCANCODE_RCTRL,  5, 1},
};

#define KEYMAP_SIZE (sizeof(keymap) / sizeof(keymap[0]))

void msx2_handle_key(MSX2* m, SDL_Scancode key, bool pressed) {
    // F9 = turbo toggle
    if (key == SDL_SCANCODE_F9 && pressed) {
        m->turbo_mode = !m->turbo_mode;
        printf("[EMU] Velocidad %s\n", m->turbo_mode ? "MAXIMA" : "normal");
        return;
    }

    // Joystick mapping: numpad keys
    if (key == SDL_SCANCODE_KP_8 || key == SDL_SCANCODE_KP_2 ||
        key == SDL_SCANCODE_KP_4 || key == SDL_SCANCODE_KP_6 ||
        key == SDL_SCANCODE_KP_0) {
        uint8_t bit = 0;
        switch (key) {
        case SDL_SCANCODE_KP_8: bit = 0x01; break; // up
        case SDL_SCANCODE_KP_2: bit = 0x02; break; // down
        case SDL_SCANCODE_KP_4: bit = 0x04; break; // left
        case SDL_SCANCODE_KP_6: bit = 0x08; break; // right
        case SDL_SCANCODE_KP_0: bit = 0x10; break; // trigger A
        default: break;
        }
        if (pressed) m->joy1 &= ~bit; // active low
        else         m->joy1 |= bit;
        return;
    }

    for (int i = 0; i < (int)KEYMAP_SIZE; i++) {
        if (keymap[i].sc == key) {
            if (pressed) m->keyboard[keymap[i].row] &= ~(1 << keymap[i].bit);
            else         m->keyboard[keymap[i].row] |=  (1 << keymap[i].bit);
            return;
        }
    }
}

// =============================================================================
// BIOS trapping - check PC for intercepted calls
// =============================================================================

static bool check_bios_trap(MSX2* m) {
    uint16_t pc = m->cpu.pc;

    // CAS tape BIOS traps (only when BIOS is in page 0 = slot 0)
    if ((m->mem.primary_sel & 0x03) == 0 && m->cas.loaded) {
        switch (pc) {
        case 0x00E1: // TAPION
            cas_trap_tapion(m);
            // RET
            m->cpu.pc = mem_read(m, m->cpu.sp) | (mem_read(m, m->cpu.sp + 1) << 8);
            m->cpu.sp += 2;
            return true;
        case 0x00E4: // TAPIN
            cas_trap_tapin(m);
            m->cpu.pc = mem_read(m, m->cpu.sp) | (mem_read(m, m->cpu.sp + 1) << 8);
            m->cpu.sp += 2;
            return true;
        case 0x00E7: // TAPIOF
            m->cpu.f &= ~0x01;
            m->cpu.pc = mem_read(m, m->cpu.sp) | (mem_read(m, m->cpu.sp + 1) << 8);
            m->cpu.sp += 2;
            return true;
        }
    }

    // Disk BIOS traps (when disk ROM is in page 1 = slot 3-1)
    if (m->mem.has_diskrom && m->dsk.loaded) {
        int ps1 = (m->mem.primary_sel >> 2) & 3;
        int ss1 = m->mem.expanded[ps1] ? (m->mem.secondary_sel[ps1] >> 2) & 3 : 0;
        if (ps1 == 3 && ss1 == 1) {
            switch (pc) {
            case 0x4010: // DSKIO
                dsk_trap_dskio(m);
                m->cpu.pc = mem_read(m, m->cpu.sp) | (mem_read(m, m->cpu.sp + 1) << 8);
                m->cpu.sp += 2;
                return true;
            case 0x4013: // DSKCHG
                dsk_trap_dskchg(m);
                m->cpu.pc = mem_read(m, m->cpu.sp) | (mem_read(m, m->cpu.sp + 1) << 8);
                m->cpu.sp += 2;
                return true;
            case 0x4016: // GETDPB
                dsk_trap_getdpb(m);
                m->cpu.pc = mem_read(m, m->cpu.sp) | (mem_read(m, m->cpu.sp + 1) << 8);
                m->cpu.sp += 2;
                return true;
            }
        }
    }
    return false;
}

// =============================================================================
// Frame loop
// =============================================================================

void msx2_run_frame(MSX2* m) {
    int cycles_done = 0;
    int next_sample_at = MSX_TSTATES_FRAME / MSX_SAMPLES_FRAME;
    int scanline_cycles = 0;
    int current_line = 0;

    // VBLANK interrupt at start of frame
    if (m->vdp.regs[1] & 0x20) { // IE0 enabled
        m->vdp.status[0] |= 0x80; // set VBLANK flag
        m->vdp.irq_vblank = true;
    }

    if (m->vdp.irq_vblank || m->vdp.irq_hblank)
        z80_pulse_irq(&m->cpu, 0xFF);

    while (cycles_done < MSX_TSTATES_FRAME) {
        // BIOS trapping
        if (check_bios_trap(m)) continue;

        int cyc = (int)z80_step(&m->cpu);
        cycles_done += cyc;
        scanline_cycles += cyc;

        // PSG advance
        psg_advance(&m->psg, cyc);

        // Scanline timing
        while (scanline_cycles >= MSX_CLOCKS_PER_LINE) {
            scanline_cycles -= MSX_CLOCKS_PER_LINE;
            if (current_line < MSX_FULL_H)
                vdp_render_line(&m->vdp, current_line, m->framebuffer);
            current_line++;

            // Horizontal interrupt (line compare)
            if (current_line == (m->vdp.regs[19] + MSX_BORDER_V) &&
                (m->vdp.regs[0] & 0x10)) {
                m->vdp.status[1] |= 0x01;
                m->vdp.irq_hblank = true;
                z80_pulse_irq(&m->cpu, 0xFF);
            }
        }

        // Audio sampling
        while (cycles_done >= next_sample_at) {
            if (m->audio_pos < MSX_SAMPLES_FRAME) {
                float sample = psg_sample(&m->psg);
                // Add beeper (PPI port C bit 7)
                if (m->ppi.port_c & 0x80) sample += 0.1f;
                m->audio_buffer[m->audio_pos++] = sample * 0.5f;
            }
            next_sample_at += MSX_TSTATES_FRAME / MSX_SAMPLES_FRAME;
        }
    }

    // Render remaining scanlines
    while (current_line < MSX_FULL_H) {
        vdp_render_line(&m->vdp, current_line, m->framebuffer);
        current_line++;
    }

    // Queue audio
    if (!m->turbo_mode && m->audio_dev > 0 && m->audio_pos > 0) {
        SDL_QueueAudio(m->audio_dev, m->audio_buffer, m->audio_pos * sizeof(float));
        m->audio_pos = 0;
    } else {
        m->audio_pos = 0;
    }

    m->vdp.frame_counter++;
}

// =============================================================================
// Presentación SDL
// =============================================================================

void msx2_render(MSX2* m) {
    SDL_UpdateTexture(m->texture, NULL, m->framebuffer, MSX_FULL_W * sizeof(uint32_t));
    SDL_RenderClear(m->renderer);
    SDL_Rect dst = { 0, 0, MSX_FULL_W * MSX_SCALE, MSX_FULL_H * MSX_SCALE };
    SDL_RenderCopy(m->renderer, m->texture, NULL, &dst);
    SDL_RenderPresent(m->renderer);
}

// =============================================================================
// Utilidades
// =============================================================================

static bool ext_eq(const char* path, const char* ext) {
    size_t plen = strlen(path), elen = strlen(ext);
    if (plen < elen) return false;
    const char* tail = path + plen - elen;
    for (size_t i = 0; i < elen; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    const char* rom_dir = ".";
    const char* media_file = NULL;

    if (argc < 2) {
        printf("Uso: %s [rom_dir] [archivo.rom|.cas|.dsk]\n", argv[0]);
        printf("  rom_dir: directorio con MSX2.ROM, MSX2EXT.ROM (y opcionalmente DISK.ROM)\n");
        printf("  F9 -> velocidad maxima / normal\n");
        printf("  Numpad: 8/2/4/6 = arriba/abajo/izq/der, 0 = disparo\n");
        printf("  Cursores = cursores MSX\n");
    }

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (ext_eq(argv[i], ".rom") || ext_eq(argv[i], ".cas") || ext_eq(argv[i], ".dsk"))
            media_file = argv[i];
        else
            rom_dir = argv[i];
    }

    msx2_init(&msx);

    if (msx2_load_bios(&msx, rom_dir) != 0) {
        fprintf(stderr, "Error: no se encontraron BIOS ROMs en '%s'\n", rom_dir);
        msx2_destroy(&msx);
        return 1;
    }

    if (media_file) {
        if (ext_eq(media_file, ".rom")) {
            if (msx2_load_rom(&msx, media_file) != 0)
                fprintf(stderr, "Error cargando ROM: %s\n", media_file);
        } else if (ext_eq(media_file, ".cas")) {
            if (msx2_load_cas(&msx, media_file) != 0)
                fprintf(stderr, "Error cargando CAS: %s\n", media_file);
            else
                printf("CAS cargado. Escribe RUN\"CAS:\" o BLOAD\"CAS:\",R en BASIC.\n");
        } else if (ext_eq(media_file, ".dsk")) {
            if (msx2_load_dsk(&msx, media_file) != 0)
                fprintf(stderr, "Error cargando DSK: %s\n", media_file);
            else
                printf("DSK cargado.\n");
        }
    }

    const uint32_t FRAME_MS = 1000 / 50;

    while (!msx.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: msx.quit = true; break;
            case SDL_KEYDOWN:
                msx2_handle_key(&msx, ev.key.keysym.scancode, true); break;
            case SDL_KEYUP:
                msx2_handle_key(&msx, ev.key.keysym.scancode, false); break;
            }
        }

        msx2_run_frame(&msx);
        msx2_render(&msx);

        if (!msx.turbo_mode) {
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS)
                SDL_Delay(FRAME_MS - elapsed);
        }
    }

    msx2_destroy(&msx);
    return 0;
}
