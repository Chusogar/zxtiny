#include "shaolins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────────────────────
// Utilidades ROM
// ─────────────────────────────────────────────────────────────────────────────
static int load_rom_exact(const char *dir, const char *file,
                          uint8_t *dst, size_t off, size_t cap, size_t expect)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0 || (size_t)sz != expect || off + (size_t)sz > cap) {
        fclose(fp);
        return -2;
    }
    fread(dst + off, 1, (size_t)sz, fp);
    fclose(fp);
    printf("[ROM] %s (%ld) -> off=0x%zx\n", path, sz, off);
    return 0;
}

static int load_rom_any(const char *dir, const char **names, int n,
                        uint8_t *dst, size_t off, size_t cap, size_t expect)
{
    for (int i = 0; i < n; i++) {
        if (load_rom_exact(dir, names[i], dst, off, cap, expect) == 0)
            return 0;
    }
    fprintf(stderr, "[ROM] No encontrada (dir=%s) ninguna variante para offset 0x%zx size=0x%zx\n",
            dir, off, expect);
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// SN76489
// ─────────────────────────────────────────────────────────────────────────────
static void sn_reset(SN76489 *s) {
    memset(s, 0, sizeof(*s));
    s->volume[0] = s->volume[1] = s->volume[2] = s->volume[3] = 15;
    s->tone_out[0] = s->tone_out[1] = s->tone_out[2] = 1;
    s->noise_lfsr = 0x8000;
    s->noise_out  = 1;
    s->noise_period = 0x10;
}

static void sn_write(SN76489 *s, uint8_t val) {
    if (val & 0x80) {
        s->latch_reg = (val >> 4) & 0x07;
        uint8_t d4 = val & 0x0F;
        switch (s->latch_reg) {
        case 0: s->tone_period[0] = (s->tone_period[0] & 0x3F0) | d4; break;
        case 1: s->volume[0] = d4; break;
        case 2: s->tone_period[1] = (s->tone_period[1] & 0x3F0) | d4; break;
        case 3: s->volume[1] = d4; break;
        case 4: s->tone_period[2] = (s->tone_period[2] & 0x3F0) | d4; break;
        case 5: s->volume[2] = d4; break;
        case 6:
            s->noise_ctrl = d4 & 0x07;
            s->noise_lfsr = 0x8000;
            switch (s->noise_ctrl & 0x03) {
            case 0: s->noise_period = 0x10; break;
            case 1: s->noise_period = 0x20; break;
            case 2: s->noise_period = 0x40; break;
            case 3: s->noise_period = s->tone_period[2] ? s->tone_period[2] : 1; break;
            }
            break;
        case 7: s->volume[3] = d4; break;
        }
    } else {
        uint8_t d6 = val & 0x3F;
        switch (s->latch_reg) {
        case 0: s->tone_period[0] = (s->tone_period[0] & 0x00F) | ((uint16_t)d6 << 4); break;
        case 2: s->tone_period[1] = (s->tone_period[1] & 0x00F) | ((uint16_t)d6 << 4); break;
        case 4:
            s->tone_period[2] = (s->tone_period[2] & 0x00F) | ((uint16_t)d6 << 4);
            if ((s->noise_ctrl & 0x03) == 3)
                s->noise_period = s->tone_period[2] ? s->tone_period[2] : 1;
            break;
        default: break;
        }
    }
}

static float sn_sample(SN76489 *s, int ticks) {
    static const float vol_lut[16] = {
        1.0000f, 0.7943f, 0.6310f, 0.5012f, 0.3981f, 0.3162f, 0.2512f, 0.1995f,
        0.1585f, 0.1259f, 0.1000f, 0.0794f, 0.0631f, 0.0501f, 0.0398f, 0.0000f
    };

    float out = 0.0f;

    for (int ch = 0; ch < 3; ch++) {
        uint16_t p = s->tone_period[ch];
        if (p < 1) p = 1;
        s->tone_counter[ch] += (uint16_t)ticks;
        while (s->tone_counter[ch] >= p) {
            s->tone_counter[ch] -= p;
            s->tone_out[ch] = -s->tone_out[ch];
        }
        out += (float)s->tone_out[ch] * vol_lut[s->volume[ch]];
    }

    {
        uint16_t np = s->noise_period ? s->noise_period : 1;
        s->noise_counter += (uint16_t)ticks;
        while (s->noise_counter >= np) {
            s->noise_counter -= np;
            int fb = (s->noise_ctrl & 0x04)
                ? ((s->noise_lfsr & 1) ^ ((s->noise_lfsr >> 1) & 1))
                :  (s->noise_lfsr & 1);
            s->noise_out  = (s->noise_lfsr & 1) ? 1 : -1;
            s->noise_lfsr = (s->noise_lfsr >> 1) | ((uint16_t)fb << 14);
        }
        out += (float)s->noise_out * vol_lut[s->volume[3]];
    }

    return out / 4.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio callback
// ─────────────────────────────────────────────────────────────────────────────
static void audio_cb(void *userdata, Uint8 *stream, int len) {
    Shaolins *s = (Shaolins*)userdata;
    float *out = (float*)stream;
    int samples = len / (int)sizeof(float);

    int t0 = (SHAO_SN1_CLOCK / SHAO_AUDIO_RATE) / 16;
    int t1 = (SHAO_SN2_CLOCK / SHAO_AUDIO_RATE) / 16;
    if (t0 < 1) t0 = 1;
    if (t1 < 1) t1 = 1;

    for (int i = 0; i < samples; i++) {
        float v = 0.0f;
        v += sn_sample(&s->sn[0], t0);
        v += sn_sample(&s->sn[1], t1);
        out[i] = v * 0.25f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GFX decode (MAME layout) - MSB-first
// ─────────────────────────────────────────────────────────────────────────────
static inline int bit_get(const uint8_t *src, int bit_index) {
    // bit 0 == MSB (0x80)
    return (src[bit_index >> 3] >> (7 - (bit_index & 7))) & 1;
}

static void decode_chars(Shaolins *s) {
    static const int plane[4] = { 512*16*8 + 4, 512*16*8 + 0, 4, 0 };
    static const int xoff[8]  = { 0,1,2,3, 8*8+0, 8*8+1, 8*8+2, 8*8+3 };
    static const int yoff[8]  = { 0*8,1*8,2*8,3*8,4*8,5*8,6*8,7*8 };
    const int charinc = 16*8;

    for (int c = 0; c < SHAO_NUM_CHARS; c++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int pen = 0;
                for (int p = 0; p < 4; p++) {
                    int b = plane[p] + c*charinc + yoff[y] + xoff[x];
                    pen |= bit_get(s->gfx1, b) << p;
                }
                s->char_pix[c][y][x] = (uint8_t)pen;
            }
        }
    }
}

static void decode_sprites(Shaolins *s) {
    static const int plane[4] = { 256*64*8 + 4, 256*64*8 + 0, 4, 0 };
    static const int xoff[16] = {
        0,1,2,3,
        8*8+0, 8*8+1, 8*8+2, 8*8+3,
        16*8+0,16*8+1,16*8+2,16*8+3,
        24*8+0,24*8+1,24*8+2,24*8+3
    };
    static const int yoff[16] = {
        0*8,1*8,2*8,3*8,4*8,5*8,6*8,7*8,
        32*8,33*8,34*8,35*8,36*8,37*8,38*8,39*8
    };
    const int sprinc = 64*8;

    for (int n = 0; n < SHAO_NUM_SPRITES; n++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int pen = 0;
                for (int p = 0; p < 4; p++) {
                    int b = plane[p] + n*sprinc + yoff[y] + xoff[x];
                    pen |= bit_get(s->gfx2, b) << p;
                }
                s->spr_pix[n][y][x] = (uint8_t)pen;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Paleta y colortables
// ─────────────────────────────────────────────────────────────────────────────
static void build_palette_and_ct(Shaolins *s) {
    for (int i = 0; i < 256; i++) {
        int bit0,bit1,bit2,bit3;
        int r,g,b;

        bit0 = (s->proms[0x000 + i] >> 0) & 1;
        bit1 = (s->proms[0x000 + i] >> 1) & 1;
        bit2 = (s->proms[0x000 + i] >> 2) & 1;
        bit3 = (s->proms[0x000 + i] >> 3) & 1;
        r = 0x0e*bit0 + 0x1f*bit1 + 0x43*bit2 + 0x8f*bit3;

        bit0 = (s->proms[0x100 + i] >> 0) & 1;
        bit1 = (s->proms[0x100 + i] >> 1) & 1;
        bit2 = (s->proms[0x100 + i] >> 2) & 1;
        bit3 = (s->proms[0x100 + i] >> 3) & 1;
        g = 0x0e*bit0 + 0x1f*bit1 + 0x43*bit2 + 0x8f*bit3;

        bit0 = (s->proms[0x200 + i] >> 0) & 1;
        bit1 = (s->proms[0x200 + i] >> 1) & 1;
        bit2 = (s->proms[0x200 + i] >> 2) & 1;
        bit3 = (s->proms[0x200 + i] >> 3) & 1;
        b = 0x0e*bit0 + 0x1f*bit1 + 0x43*bit2 + 0x8f*bit3;

        s->pal_argb[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    const uint8_t *chr_lut = &s->proms[0x300];
    const uint8_t *spr_lut = &s->proms[0x400];

    for (int i = 0; i < 256; i++) {
        for (int bank = 0; bank < 8; bank++) {
            s->ct_char[i + bank*256] = (chr_lut[i] & 0x0F) + 32*bank + 16;
        }
    }

    for (int i = 0; i < 256; i++) {
        for (int bank = 0; bank < 8; bank++) {
            uint8_t nib = spr_lut[i] & 0x0F;
            s->ct_spr[i + bank*256] = (nib == 0) ? 0 : (uint8_t)(nib + 32*bank);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Video helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline void put_raw(Shaolins *s, int x, int y, uint32_t argb) {
    if ((unsigned)x < SHAO_RAW_W && (unsigned)y < SHAO_RAW_H)
        s->raw[y*SHAO_RAW_W + x] = argb;
}

static void clear_raw(Shaolins *s) {
    for (int i = 0; i < SHAO_RAW_W*SHAO_RAW_H; i++)
        s->raw[i] = 0xFF000000u;
}

static void draw_bg_tile(Shaolins *s, int tile_index, int sx, int sy) {
    int attr  = s->colorram[tile_index];
    int code  = s->videoram[tile_index] + ((attr & 0x40) << 2);
    int color = (attr & 0x0F) + 16*(s->palettebank & 7);
    bool flipy = (attr & 0x20) != 0;

    bool flip_global = (s->nmi_enable & 0x01) != 0;

    for (int y = 0; y < 8; y++) {
        int yy = flipy ? (7 - y) : y;
        for (int x = 0; x < 8; x++) {
            int xx = x;

            int px = sx + xx;
            int py = sy + y;

            if (flip_global) {
                px = 255 - px;
                py = 255 - py;
                yy = 7 - yy;
                xx = 7 - xx;
            }

            uint8_t pen = s->char_pix[code & (SHAO_NUM_CHARS-1)][yy][xx];
            uint8_t palidx = s->ct_char[(color & 0x7F)*16 + (pen & 0x0F)];
            put_raw(s, px, py, s->pal_argb[palidx]);
        }
    }
}

static void draw_sprite(Shaolins *s, int code, int color, bool flipx, bool flipy, int sx, int sy) {
    bool flip_global = (s->nmi_enable & 0x01) != 0;

    if (flip_global) {
        sx = 240 - sx;
        sy = 248 - sy;
        flipx = !flipx;
        flipy = !flipy;
    }

    for (int y = 0; y < 16; y++) {
        int yy = flipy ? (15 - y) : y;
        for (int x = 0; x < 16; x++) {
            int xx = flipx ? (15 - x) : x;
            uint8_t pen = s->spr_pix[code & 0xFF][yy][xx];
            if (pen == 0) continue;

            uint8_t palidx = s->ct_spr[(color & 0x7F)*16 + (pen & 0x0F)];
            if (palidx == 0) continue;
            put_raw(s, sx + x, sy + y, s->pal_argb[palidx]);
        }
    }
}

static void video_refresh(Shaolins *s) {
    clear_raw(s);

    for (int col = 0; col < 32; col++) {
        uint8_t sc = s->scrolly[col];
        for (int row = 0; row < 32; row++) {
            int idx = row*32 + col;
            int sx = col * 8;
            int sy = ((row * 8) - (int)sc) & 0xFF;
            draw_bg_tile(s, idx, sx, sy);
        }
    }

    for (int offs = 0x300 - 32; offs >= 0; offs -= 32) {
        if (s->sprram[offs] && s->sprram[offs + 6]) {
            int  code  = s->sprram[offs + 8];
            int  color = (s->sprram[offs + 9] & 0x0F) + 16*(s->palettebank & 7);
            bool flipx = !(s->sprram[offs + 9] & 0x40);
            bool flipy =  (s->sprram[offs + 9] & 0x80) != 0;
            int sx = 240 - s->sprram[offs + 6];
            int sy = 248 - s->sprram[offs + 4];

            draw_sprite(s, code, color, flipx, flipy, sx, sy);
        }
    }

    for (int y = 0; y < SHAO_VIS_H; y++) {
        memcpy(&s->vis[y*SHAO_VIS_W],
               &s->raw[(y + SHAO_VIS_Y0)*SHAO_RAW_W],
               SHAO_VIS_W * sizeof(uint32_t));
    }

    if (s->rotate90) {
        for (int y = 0; y < SHAO_VIS_H; y++) {
            for (int x = 0; x < SHAO_VIS_W; x++) {
                int dx = (SHAO_VIS_H - 1) - y;
                int dy = x;
                s->out[dy * SHAO_ROT_W + dx] = s->vis[y * SHAO_VIS_W + x];
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU bus
// ─────────────────────────────────────────────────────────────────────────────
static mc6809byte__t cpu_read(mc6809__t *cpu, mc6809addr__t addr, bool ifetch) {
    (void)ifetch;
    Shaolins *s = (Shaolins*)cpu->user;

    switch (addr) {
    case 0x0500: return s->dsw1;
    case 0x0600: return s->dsw2;
    case 0x0700: return s->in_system;
    case 0x0701: return s->in_p1;
    case 0x0702: return s->in_p2;
    case 0x0703: return s->dsw3;
    default: break;
    }

    if (addr >= 0x2800 && addr <= 0x2BFF) return s->ram2[addr - 0x2800];
    if (addr >= 0x3000 && addr <= 0x30FF) return s->ram1[addr - 0x3000];
    if (addr >= 0x3100 && addr <= 0x33FF) return s->sprram[addr - 0x3100];
    if (addr >= 0x3800 && addr <= 0x3BFF) return s->colorram[addr - 0x3800];
    if (addr >= 0x3C00 && addr <= 0x3FFF) return s->videoram[addr - 0x3C00];

    return s->mem[addr];
}

static void cpu_write(mc6809__t *cpu, mc6809addr__t addr, mc6809byte__t val) {
    Shaolins *s = (Shaolins*)cpu->user;

    if (addr == 0x0000) { s->nmi_enable = (uint8_t)val; return; }
    if (addr == 0x0100) { return; }

    // ─────────────────────────────────────────────────────────────────────
    // SONIDO (FIX):
    // MAME mapea 0x0300/0x0400 como escritura al chip SN (trigger).
    // 0x0800/0x1000 son latches (NOP en MAME porque el juego suele escribir lo mismo).
    // Por compatibilidad, aquí: latch guarda val, y 0x0300/0x0400 escriben *val* al chip.
    // ─────────────────────────────────────────────────────────────────────
    if (addr == 0x0800) { s->sn_latch0 = (uint8_t)val; return; }
    if (addr == 0x1000) { s->sn_latch1 = (uint8_t)val; return; }

    if (addr == 0x0300) {
        // escribe directamente al SN0; además actualiza latch por si el juego lo usa así
        s->sn_latch0 = (uint8_t)val;
        sn_write(&s->sn[0], (uint8_t)val);
        return;
    }
    if (addr == 0x0400) {
        s->sn_latch1 = (uint8_t)val;
        sn_write(&s->sn[1], (uint8_t)val);
        return;
    }

    if (addr == 0x1800) { s->palettebank = (uint8_t)(val & 0x07); return; }

    if (addr == 0x2000) {
        s->scroll = (uint8_t)val;
        for (int c = 0; c < 32; c++) s->scrolly[c] = 0;
        for (int c = 4; c < 32; c++) s->scrolly[c] = (uint8_t)(val + 1);
        return;
    }

    if (addr >= 0x2800 && addr <= 0x2BFF) { s->ram2[addr - 0x2800] = (uint8_t)val; return; }
    if (addr >= 0x3000 && addr <= 0x30FF) { s->ram1[addr - 0x3000] = (uint8_t)val; return; }
    if (addr >= 0x3100 && addr <= 0x33FF) { s->sprram[addr - 0x3100] = (uint8_t)val; return; }
    if (addr >= 0x3800 && addr <= 0x3BFF) { s->colorram[addr - 0x3800] = (uint8_t)val; return; }
    if (addr >= 0x3C00 && addr <= 0x3FFF) { s->videoram[addr - 0x3C00] = (uint8_t)val; return; }
}

static void cpu_fault(mc6809__t *cpu, mc6809fault__t fault) {
    fprintf(stderr, "[MC6809] Fault %d en PC=$%04X\n", fault, (unsigned)cpu->instpc);
}

// ─────────────────────────────────────────────────────────────────────────────
// API
// ─────────────────────────────────────────────────────────────────────────────
void shaolins_init(Shaolins *s) {
    memset(s, 0, sizeof(*s));

    s->in_system = 0xFF;
    s->in_p1     = 0xFF;
    s->in_p2     = 0xFF;

    s->dsw1 = (uint8_t)(0x02 | 0x18 | 0x40);
    s->dsw2 = 0x07;
    s->dsw3 = 0xFF;

    s->rotate90 = true;

    for (int c = 0; c < 32; c++) s->scrolly[c] = 0;

    sn_reset(&s->sn[0]);
    sn_reset(&s->sn[1]);
    s->sn_latch0 = s->sn_latch1 = 0;

    memset(&s->cpu, 0, sizeof(s->cpu));
    s->cpu.read  = cpu_read;
    s->cpu.write = cpu_write;
    s->cpu.fault = cpu_fault;
    s->cpu.user  = s;
}

void shaolins_destroy(Shaolins *s) {
    free(s->char_pix);
    free(s->spr_pix);

    if (s->audio_dev) SDL_CloseAudioDevice(s->audio_dev);
    if (s->texture) SDL_DestroyTexture(s->texture);
    if (s->renderer) SDL_DestroyRenderer(s->renderer);
    if (s->window) SDL_DestroyWindow(s->window);
    SDL_Quit();
}

int shaolins_load_roms(Shaolins *s, const char *dir) {
    memset(s->mem,  0xFF, sizeof(s->mem));
    memset(s->gfx1, 0x00, sizeof(s->gfx1));
    memset(s->gfx2, 0x00, sizeof(s->gfx2));
    memset(s->proms,0x00, sizeof(s->proms));

    const char *rom0[] = { "477-l03.d9", "kikrd8.bin"  };
    const char *rom1[] = { "477-l04.d10","kikrd9.bin"  };
    const char *rom2[] = { "477-l05.d11","kikrd11.bin" };

    const char *g1a[]  = { "477j06.a10", "shaolins.6", "shaolins.a10", "kikra10.bin" };
    const char *g1b[]  = { "477j07.a11", "shaolins.7", "shaolins.a11", "kikra11.bin" };

    const char *g2a[]  = { "477j02.h15", "kikrh14.bin" };
    const char *g2b[]  = { "477j01.h14", "kikrh13.bin" };

    const char *pr[]   = { "477j10.a12", "kicker.a12" };
    const char *pg[]   = { "477j11.a13", "kicker.a13" };
    const char *pb[]   = { "477j12.a14", "kicker.a14" };
    const char *plc[]  = { "477j09.b8",  "kicker.b8"  };
    const char *pls[]  = { "477j08.f16", "kicker.f16" };

    int e = 0;

    e |= load_rom_any(dir, rom0, 2, s->mem,  0x6000, sizeof(s->mem),  0x2000);
    e |= load_rom_any(dir, rom1, 2, s->mem,  0x8000, sizeof(s->mem),  0x4000);
    e |= load_rom_any(dir, rom2, 2, s->mem,  0xC000, sizeof(s->mem),  0x4000);

    e |= load_rom_any(dir, g1a, 4, s->gfx1, 0x0000, sizeof(s->gfx1), 0x2000);
    e |= load_rom_any(dir, g1b, 4, s->gfx1, 0x2000, sizeof(s->gfx1), 0x2000);

    e |= load_rom_any(dir, g2a, 2, s->gfx2, 0x0000, sizeof(s->gfx2), 0x4000);
    e |= load_rom_any(dir, g2b, 2, s->gfx2, 0x4000, sizeof(s->gfx2), 0x4000);

    e |= load_rom_any(dir, pr,  2, s->proms, 0x000, sizeof(s->proms), 0x100);
    e |= load_rom_any(dir, pg,  2, s->proms, 0x100, sizeof(s->proms), 0x100);
    e |= load_rom_any(dir, pb,  2, s->proms, 0x200, sizeof(s->proms), 0x100);
    e |= load_rom_any(dir, plc, 2, s->proms, 0x300, sizeof(s->proms), 0x100);
    e |= load_rom_any(dir, pls, 2, s->proms, 0x400, sizeof(s->proms), 0x100);

    if (e) return -1;

    s->char_pix = (ShaoCharPix*)calloc(SHAO_NUM_CHARS, sizeof(ShaoCharPix));
    s->spr_pix  = (ShaoSprPix*) calloc(SHAO_NUM_SPRITES, sizeof(ShaoSprPix));
    if (!s->char_pix || !s->spr_pix) {
        fprintf(stderr, "[SHAO] OOM decodificando GFX\n");
        return -1;
    }

    decode_chars(s);
    decode_sprites(s);
    build_palette_and_ct(s);

    printf("[SHAO] ROMs OK en '%s'\n", dir);
    return 0;
}

void shaolins_run_frame(Shaolins *s) {
    const int cycles_per_frame = (int)((float)SHAO_CPU_CLOCK / SHAO_FPS);
    const int slices   = 16;
    const int cyc_slice = cycles_per_frame / slices;

    for (int sl = 0; sl < slices; sl++) {
        int ran = 0;
        while (ran < cyc_slice) {
            unsigned long before = s->cpu.cycles;
            mc6809_step(&s->cpu);
            ran += (int)(s->cpu.cycles - before);
        }

        if (sl == slices - 1) {
            s->cpu.irq = true;
            mc6809_step(&s->cpu);
            s->cpu.irq = false;
        } else if ((sl & 1) && (s->nmi_enable & 0x02)) {
            s->cpu.irq = true;
            mc6809_step(&s->cpu);
            s->cpu.irq = false;
        }
    }

    video_refresh(s);
}

void shaolins_render(Shaolins *s) {
    const uint32_t *pix = s->rotate90 ? s->out : s->vis;
    int w = s->rotate90 ? SHAO_ROT_W : SHAO_VIS_W;
    int h = s->rotate90 ? SHAO_ROT_H : SHAO_VIS_H;

    SDL_UpdateTexture(s->texture, NULL, pix, w * (int)sizeof(uint32_t));
    SDL_RenderClear(s->renderer);
    SDL_Rect dst = { 0, 0, w * SHAO_SCALE, h * SHAO_SCALE };
    SDL_RenderCopy(s->renderer, s->texture, NULL, &dst);
    SDL_RenderPresent(s->renderer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Inputs
// ─────────────────────────────────────────────────────────────────────────────
static void set_bit_low(uint8_t *reg, uint8_t mask, bool pressed) {
    if (pressed) *reg &= (uint8_t)~mask;
    else         *reg |= mask;
}

void shaolins_handle_key(Shaolins *s, SDL_Scancode sc, bool dn) {
    switch (sc) {
    case SDL_SCANCODE_5:  set_bit_low(&s->in_system, 0x01, dn); break;
    case SDL_SCANCODE_6:  set_bit_low(&s->in_system, 0x02, dn); break;
    //case SDL_SCANCODE_F2: set_bit_low(&s->in_system, 0x04, dn); break;
    case SDL_SCANCODE_1:  set_bit_low(&s->in_system, 0x08, dn); break;
    case SDL_SCANCODE_2:  set_bit_low(&s->in_system, 0x10, dn); break;

    case SDL_SCANCODE_LEFT:  set_bit_low(&s->in_p1, 0x01, dn); break;
    case SDL_SCANCODE_RIGHT: set_bit_low(&s->in_p1, 0x02, dn); break;
    case SDL_SCANCODE_UP:    set_bit_low(&s->in_p1, 0x04, dn); break;
    case SDL_SCANCODE_DOWN:  set_bit_low(&s->in_p1, 0x08, dn); break;
    case SDL_SCANCODE_Z:     set_bit_low(&s->in_p1, 0x10, dn); break;
    case SDL_SCANCODE_X:     set_bit_low(&s->in_p1, 0x20, dn); break;

    case SDL_SCANCODE_A: set_bit_low(&s->in_p2, 0x01, dn); break;
    case SDL_SCANCODE_D: set_bit_low(&s->in_p2, 0x02, dn); break;
    case SDL_SCANCODE_W: set_bit_low(&s->in_p2, 0x04, dn); break;
    case SDL_SCANCODE_S: set_bit_low(&s->in_p2, 0x08, dn); break;
    case SDL_SCANCODE_Q: set_bit_low(&s->in_p2, 0x10, dn); break;
    case SDL_SCANCODE_E: set_bit_low(&s->in_p2, 0x20, dn); break;

    case SDL_SCANCODE_R:
        if (dn) {
            s->rotate90 = !s->rotate90;
            int w = s->rotate90 ? SHAO_ROT_W : SHAO_VIS_W;
            int h = s->rotate90 ? SHAO_ROT_H : SHAO_VIS_H;
            if (s->texture) SDL_DestroyTexture(s->texture);
            s->texture = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
        }
        break;
	
	// Turbo
	case SDL_SCANCODE_F2:
		if (dn) {
			s->turbo_mode = !s->turbo_mode;
			//if (!t->turbo_mode && t->audio_dev > 0) SDL_ClearQueuedAudio(t->audio_dev);
			printf("[EMU] Velocidad %s\n", s->turbo_mode ? "MAXIMA" : "normal");
		}
		return;

    case SDL_SCANCODE_ESCAPE:
        if (dn) s->quit = true;
        break;

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
static void print_usage(const char *exe) {
    printf("Uso: %s <rom_dir>\n", exe);
    printf("Emulador Shao-lin's Road (shaolins) - estilo MAME 0.37b7\n");
    printf("Teclas:\n");
    printf("  Flechas + Z/X    P1\n");
    printf("  WASD + Q/E       P2\n");
    printf("  1/2              Start\n");
    printf("  5/6              Coin\n");
    printf("  R                Toggle ROT90\n");
    printf("  ESC              Salir\n");
}

int main(int argc, char **argv) {
    const char *rom_dir = (argc > 1) ? argv[1] : ".";
    Shaolins sh;
    shaolins_init(&sh);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    if (shaolins_load_roms(&sh, rom_dir) != 0) {
        fprintf(stderr, "[SHAO] Error cargando ROMs\n");
        print_usage(argv[0]);
        shaolins_destroy(&sh);
        return 1;
    }

    int w = sh.rotate90 ? SHAO_ROT_W : SHAO_VIS_W;
    int h = sh.rotate90 ? SHAO_ROT_H : SHAO_VIS_H;

    sh.window = SDL_CreateWindow("Shao-lin's Road (shaolins) - SDL2",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 w * SHAO_SCALE, h * SHAO_SCALE, 0);
    sh.renderer = SDL_CreateRenderer(sh.window, -1, SDL_RENDERER_ACCELERATED);
    sh.texture  = SDL_CreateTexture(sh.renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = SHAO_AUDIO_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 1;
    want.samples  = SHAO_AUDIO_SAMPLES;
    want.callback = audio_cb;
    want.userdata = &sh;

    sh.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!sh.audio_dev) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice fallo: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(sh.audio_dev, 0);
        printf("[AUDIO] OK freq=%d ch=%d\n", have.freq, have.channels);
    }

    mc6809_reset(&sh.cpu);

    uint32_t last = SDL_GetTicks();
    const float ms_per_frame = 1000.0f / SHAO_FPS;

    while (!sh.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) sh.quit = true;
            if (ev.type == SDL_KEYDOWN) shaolins_handle_key(&sh, ev.key.keysym.scancode, true);
            if (ev.type == SDL_KEYUP)   shaolins_handle_key(&sh, ev.key.keysym.scancode, false);
        }

        shaolins_run_frame(&sh);
        shaolins_render(&sh);

        uint32_t now = SDL_GetTicks();
        float elapsed = (float)(now - last);
		if (!sh.turbo_mode)
		{
			if (elapsed < ms_per_frame) SDL_Delay((uint32_t)(ms_per_frame - elapsed));
		}
        
        last = SDL_GetTicks();
    }

    shaolins_destroy(&sh);
    return 0;
}