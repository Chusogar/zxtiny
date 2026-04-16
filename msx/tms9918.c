#include "tms9918.h"
#include <string.h>

// Forzar VRAM 16KB en R#1 (ColecoVision)
#define TMS_R1_16K (0x80)

// Paleta ColecoVision (igual que la que tenías en coleco.c)
static const uint32_t TMS_PAL[16] = {
    0xFF000000,0xFF000000,0xFF3EB849,0xFF74D07D,
    0xFF5955E0,0xFF8076F1,0xFFB95E51,0xFF65DBEF,
    0xFFDB6559,0xFFFF897D,0xFFCCC35E,0xFFDED087,
    0xFF3AA241,0xFFB766B5,0xFFCCCCCC,0xFFFFFFFF
};

static inline uint16_t tms_nt (tms9918_t *v){ return ((uint16_t)(v->reg[2] & 0x0F)) << 10; }
static inline uint16_t tms_ct (tms9918_t *v){ return ((uint16_t)v->reg[3]) << 6; }
static inline uint16_t tms_pg (tms9918_t *v){ return ((uint16_t)(v->reg[4] & 0x07)) << 11; }
static inline uint16_t tms_sat(tms9918_t *v){ return ((uint16_t)(v->reg[5] & 0x7F)) << 7; }
static inline uint16_t tms_spg(tms9918_t *v){ return ((uint16_t)(v->reg[6] & 0x07)) << 11; }

static inline int tms_mode(tms9918_t *v){
    // M1 = R1 bit4 (TEXT)
    // M2 = R1 bit3 (MC)
    // M3 = R0 bit1 (G2)
    int m1 = (v->reg[1] & 0x10) ? 1 : 0;
    int m2 = (v->reg[1] & 0x08) ? 1 : 0;
    int m3 = (v->reg[0] & 0x02) ? 1 : 0;

    if(m1) return 1; // TEXT
    if(m2) return 4; // MULTICOLOR
    if(m3) return 2; // GRAPHIC II
    return 0;         // GRAPHIC I
}

void tms9918_init(tms9918_t *vdp, uint32_t *framebuffer, int fb_w){
    memset(vdp, 0, sizeof(*vdp));
    vdp->fb = framebuffer;
    vdp->fb_w = fb_w;
    tms9918_reset(vdp);
}

void tms9918_reset(tms9918_t *vdp){
    // No borramos VRAM necesariamente (depende del reset real),
    // pero para emu suele ser OK limpiar:
    memset(vdp->vram, 0, sizeof(vdp->vram));
    memset(vdp->reg,  0, sizeof(vdp->reg));

    vdp->addr = 0;
    vdp->latch = 0;
    vdp->second = false;
    vdp->rdbuf = 0;
    vdp->status = 0;
    vdp->write_mode = false;
    vdp->nmi_pending = false;

    vdp->line_cyc = 0;
    vdp->cur_line = 0;

    // Fuerza 16KB VRAM en Coleco
    vdp->reg[1] |= TMS_R1_16K;
}

bool tms9918_consume_nmi(tms9918_t *vdp){
    bool p = vdp->nmi_pending;
    vdp->nmi_pending = false;
    return p;
}

// ── VDP: control port write (0xBF) ───────────────────────────────────────────
void tms9918_write_ctrl(tms9918_t *vdp, uint8_t val){
    if(!vdp->second){
        vdp->latch = val;
        vdp->second = true;
        return;
    }

    vdp->second = false;

    if(val & 0x80){
        uint8_t r = val & 7;

        if(r == 1){
            // ✅ Coleco: forzar 16KB VRAM siempre
            vdp->reg[1] = (uint8_t)(vdp->latch | TMS_R1_16K);

            // Si habilita IE (bit5) y VBlank ya está activo, pedimos NMI
            if((vdp->reg[1] & 0x20) && (vdp->status & 0x80))
                vdp->nmi_pending = true;
        } else {
            vdp->reg[r] = vdp->latch;
        }
        return;
    }

    // set VRAM address + R/W bit
    vdp->write_mode = (val & 0x40) != 0;
    vdp->addr = ((uint16_t)(val & 0x3F) << 8) | vdp->latch;

    if(!vdp->write_mode){
        // read-ahead prefetch
        vdp->rdbuf = vdp->vram[vdp->addr & 0x3FFF];
        vdp->addr  = (vdp->addr + 1) & 0x3FFF;
    }
}

// ── VDP: data port (0xBE) ───────────────────────────────────────────────────
uint8_t tms9918_read_data(tms9918_t *vdp){
    uint8_t v = vdp->rdbuf;
    vdp->rdbuf = vdp->vram[vdp->addr & 0x3FFF];
    vdp->addr  = (vdp->addr + 1) & 0x3FFF;
    vdp->second = false;
    return v;
}

void tms9918_write_data(tms9918_t *vdp, uint8_t val){
    vdp->vram[vdp->addr & 0x3FFF] = val;
    vdp->rdbuf = val;
    vdp->addr  = (vdp->addr + 1) & 0x3FFF;
    vdp->second = false;
}

// ── VDP: status read (0xBF) ──────────────────────────────────────────────────
uint8_t tms9918_read_status(tms9918_t *vdp){
    uint8_t s = vdp->status;
    // limpia VBlank + 5th + collision (bits 7..5)
    vdp->status &= 0x1F;
    vdp->second = false;
    return s;
}

// ── Render scanline (fondo + sprites) ─────────────────────────────────────────
static void tms_render_line(tms9918_t *vdp, int line){
    if(!vdp->fb || vdp->fb_w <= 0) return;
    if(line < 0 || line >= TMS_LINES_VISIBLE) return;

    uint32_t *row = vdp->fb + line * vdp->fb_w;
    uint32_t backdrop = TMS_PAL[vdp->reg[7] & 0x0F];

    // relleno inicial con backdrop
    for(int x=0;x<256 && x<vdp->fb_w;x++) row[x] = backdrop;

    int mode = tms_mode(vdp);

    uint16_t nt = tms_nt(vdp);
    uint16_t ct = tms_ct(vdp);
    uint16_t pt = tms_pg(vdp);

    int charY = line >> 3;
    int py    = line & 7;

    // TEXT (sprites off)
    if(mode == 1){
        uint32_t fg = TMS_PAL[(vdp->reg[7] >> 4) & 0x0F];
        uint32_t bg = TMS_PAL[vdp->reg[7] & 0x0F];

        for(int x=0;x<256 && x<vdp->fb_w;x++) row[x] = bg;

        for(int cx=0; cx<40; cx++){
            uint8_t nm = vdp->vram[(nt + charY*40 + cx) & 0x3FFF];
            uint8_t p  = vdp->vram[(pt + nm*8 + py) & 0x3FFF];

            for(int b=7;b>=2;b--){
                int xx = 8 + cx*6 + (7-b);
                if(xx>=0 && xx<256 && xx<vdp->fb_w)
                    row[xx] = (p & (1<<b)) ? fg : bg;
            }
        }
        return;
    }

    // MULTICOLOR
    if(mode == 4){
        int sub = ((charY & 3) * 2) + (py >> 2);
        for(int cx=0; cx<32; cx++){
            uint8_t nm = vdp->vram[(nt + charY*32 + cx) & 0x3FFF];
            uint8_t c  = vdp->vram[(pt + nm*8 + sub) & 0x3FFF];
            uint8_t left  = (c & 0x0F);
            uint8_t right = (c >> 4) & 0x0F;

            uint32_t cl = left  ? TMS_PAL[left]  : backdrop;
            uint32_t cr = right ? TMS_PAL[right] : backdrop;

            int base = cx*8;
            if(base+7 < 256 && base+7 < vdp->fb_w){
                row[base+0]=cl; row[base+1]=cl; row[base+2]=cl; row[base+3]=cl;
                row[base+4]=cr; row[base+5]=cr; row[base+6]=cr; row[base+7]=cr;
            }
        }
    }
    // GRAPHIC II (Mode 2)
    else if(mode == 2){
        int third = (charY / 8);                 // 0..2
        uint16_t chr_base = (uint16_t)(third << 8);

        uint16_t pg_base = (vdp->reg[4] & 0x04) ? 0x2000 : 0x0000;
        uint16_t ct_base = (vdp->reg[3] & 0x80) ? 0x2000 : 0x0000;

        uint16_t mask_pg = 0x00FF;
        if (vdp->reg[4] & 0x01) mask_pg |= 0x0100;
        if (vdp->reg[4] & 0x02) mask_pg |= 0x0200;

        uint8_t mask_ct_top = (uint8_t)(vdp->reg[3] & 0x7F);

        for(int cx=0; cx<32; cx++){
            uint8_t name = vdp->vram[(nt + charY*32 + cx) & 0x3FFF];
            uint16_t chr = (uint16_t)name | chr_base;

            uint16_t chr_pg = chr & mask_pg;

            uint8_t top7 = (uint8_t)((chr >> 3) & 0x7F);
            top7 &= mask_ct_top;
            uint16_t chr_ct = ((uint16_t)top7 << 3) | (chr & 0x07);

            uint16_t pa = (uint16_t)(chr_pg * 8 + py);
            uint16_t ca = (uint16_t)(chr_ct * 8 + py);

            uint8_t patt = vdp->vram[(pg_base + pa) & 0x3FFF];
            uint8_t col  = vdp->vram[(ct_base + ca) & 0x3FFF];

            uint8_t fg_i = (col >> 4) & 0x0F;
            uint8_t bg_i = (col & 0x0F);
            uint32_t fg = fg_i ? TMS_PAL[fg_i] : backdrop;
            uint32_t bg = bg_i ? TMS_PAL[bg_i] : backdrop;

            int base = cx*8;
            if(base+7 < 256 && base+7 < vdp->fb_w){
                for(int b=7;b>=0;b--)
                    row[base + (7-b)] = (patt & (1<<b)) ? fg : bg;
            }
        }
    }
    // GRAPHIC I
    else {
        for(int cx=0; cx<32; cx++){
            uint8_t nm   = vdp->vram[(nt + charY*32 + cx) & 0x3FFF];
            uint8_t patt = vdp->vram[(pt + nm*8 + py) & 0x3FFF];
            uint8_t col  = vdp->vram[(ct + (nm>>3)) & 0x3FFF];

            uint8_t fg_i = (col >> 4) & 0x0F;
            uint8_t bg_i = (col & 0x0F);
            uint32_t fg  = fg_i ? TMS_PAL[fg_i] : backdrop;
            uint32_t bg  = bg_i ? TMS_PAL[bg_i] : backdrop;

            int base = cx*8;
            if(base+7 < 256 && base+7 < vdp->fb_w){
                for(int b=7;b>=0;b--)
                    row[base + (7-b)] = (patt & (1<<b)) ? fg : bg;
            }
        }
    }

    // ── Sprites ──────────────────────────────────────────────────────────────
    uint16_t sa  = tms_sat(vdp);
    uint16_t sp  = tms_spg(vdp);
    bool mag = (vdp->reg[1] & 1) != 0;
    bool big = (vdp->reg[1] & 2) != 0;

    int size  = big ? 16 : 8;
    int msize = size * (mag ? 2 : 1);

    // No limpiamos el status (latch) en cada scanline
    bool fifth_latched = (vdp->status & 0x40) != 0;

    static uint8_t spr_mask[256];
    memset(spr_mask, 0, sizeof(spr_mask));

    int visible = 0;
    int last_sprite = 31;

    for(int s=0; s<32; s++){
        uint16_t o = (sa + s*4) & 0x3FFF;
        uint8_t raw_y = vdp->vram[o];

        if(raw_y == 0xD0) {
            last_sprite = s;
            break; // sentinel Coleco
        }

        int sy = (int)raw_y + 1;
        if(sy > 208) sy -= 256;

        if(line < sy || line >= sy + msize) {
            last_sprite = s;
            continue;
        }

        visible++;
        if(visible == 5){
            if(!fifth_latched){
                vdp->status |= 0x40;
                vdp->status = (vdp->status & 0xE0) | (s & 0x1F);
            }
            break;
        }

        int sx = (int)vdp->vram[(o+1) & 0x3FFF];
        uint8_t pat  = vdp->vram[(o+2) & 0x3FFF];
        uint8_t attr = vdp->vram[(o+3) & 0x3FFF];

        if(attr & 0x80) sx -= 32; // Early clock
        uint8_t col = attr & 0x0F;
        if(col == 0) continue; // transparente

        int py2 = (line - sy) / (mag ? 2 : 1);
        if(big) pat &= 0xFC;

        for(int px=0; px<msize; px++){
            int bx = px / (mag ? 2 : 1);
            int x = sx + px;
            if(x < 0 || x >= 256 || x >= vdp->fb_w) continue;

            int bi = big ? (((bx >> 3) * 2) + (py2 >> 3)) : 0;
            uint8_t patt = vdp->vram[(sp + pat*8 + bi*8 + (py2 & 7)) & 0x3FFF];

            if(patt & (0x80 >> (bx & 7))){
                if(spr_mask[x]) vdp->status |= 0x20; // colisión
                else { spr_mask[x] = 1; row[x] = TMS_PAL[col]; }
            }
        }
    }

    if (!fifth_latched && visible < 5) {
        vdp->status = (vdp->status & 0xE0) | (last_sprite & 0x1F);
    }
}

// ── Timing ───────────────────────────────────────────────────────────────────
void tms9918_tick(tms9918_t *vdp, int cycles){
    vdp->line_cyc += cycles;

    while(vdp->line_cyc >= TMS_CYCLES_PER_LINE){
        vdp->line_cyc -= TMS_CYCLES_PER_LINE;

        if(vdp->cur_line < TMS_LINES_VISIBLE){
            tms_render_line(vdp, vdp->cur_line);
        }

        // VBlank al final de línea 191
        if(vdp->cur_line == (TMS_LINES_VISIBLE - 1)){
            vdp->status |= 0x80;
            if(vdp->reg[1] & 0x20){
                vdp->nmi_pending = true; // NMI 1 vez por frame
            }
        }

        vdp->cur_line++;
        if(vdp->cur_line >= TMS_LINES_TOTAL) vdp->cur_line = 0;
    }
}