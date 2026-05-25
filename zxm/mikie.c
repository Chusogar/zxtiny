#include "mikie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Mikie2 mikie;

/* ──────────────────────────────────────────────────────────────────────────
 * SN76489
 * ────────────────────────────────────────────────────────────────────────── */
static void sn_reset(SN76489* s) {
    memset(s, 0, sizeof(*s));
    s->volume[0] = s->volume[1] = s->volume[2] = s->volume[3] = 15;
    s->tone_out[0] = s->tone_out[1] = s->tone_out[2] = 1;
    s->noise_lfsr = 0x8000;
    s->noise_out  = 1;
    s->noise_period = 0x10;
}

static void sn_write(SN76489* s, uint8_t val) {
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

static float sn_sample(SN76489* s, int ticks) {
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

/* ──────────────────────────────────────────────────────────────────────────
 * ROM loader
 * ────────────────────────────────────────────────────────────────────────── */
static int load_rom_exact(const char* dir, const char* file, uint8_t* dst, size_t off, size_t cap, size_t expect) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[MIKIE] No se puede abrir '%s'\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz != expect || off + (size_t)sz > cap) {
        fprintf(stderr, "[MIKIE] Tamaño incorrecto '%s' (%ld, esperado %zu)\n", path, sz, expect);
        fclose(fp);
        return -1;
    }
    fread(dst + off, 1, (size_t)sz, fp);
    fclose(fp);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * GFX decode (basado en GfxLayout del driver MAME)
 * ────────────────────────────────────────────────────────────────────────── */
static inline int bit_get(const uint8_t* src, int bit_index) {
    return (src[bit_index >> 3] >> (bit_index & 7)) & 1;
}

static void decode_chars(Mikie2* m) {
    for (int c = 0; c < MIKIE_NUM_CHARS; c++) {
        int base = c * 32;
        for (int y = 0; y < 8; y++) {
            const uint8_t* row = &m->gfx1[base + y * 4];
            for (int x = 0; x < 8; x++) {
                uint8_t b = row[x >> 1];
                uint8_t px = (x & 1) ? (b >> 4) & 0x0F : (b & 0x0F);
                m->char_pix[c][y][x] = px;
            }
        }
    }
}

static void decode_sprites_one(Mikie2* m, uint8_t out_pix[MIKIE_NUM_SPR][MIKIE_SPR_H][MIKIE_SPR_W], int base_byte_offset) {
    static const int plane[4] = { 0, 4, 256*128*8 + 0, 256*128*8 + 4 };
    static const int xoff[16] = {
        32*8+0, 32*8+1, 32*8+2, 32*8+3,
        16*8+0, 16*8+1, 16*8+2, 16*8+3,
        0,      1,      2,      3,
        48*8+0, 48*8+1, 48*8+2, 48*8+3
    };
    static const int yoff[16] = {
        0*16, 1*16, 2*16, 3*16, 4*16, 5*16, 6*16, 7*16,
        32*16,33*16,34*16,35*16,36*16,37*16,38*16,39*16
    };
    const int charinc = 128*8;

    for (int s = 0; s < MIKIE_NUM_SPR; s++) {
        int sprite_base_bit = (base_byte_offset * 8) + s * charinc;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int pix = 0;
                for (int p = 0; p < 4; p++) {
                    int bit_index = sprite_base_bit + yoff[y] + xoff[x] + plane[p];
                    pix |= bit_get(m->gfx2, bit_index) << p;
                }
                out_pix[s][y][x] = (uint8_t)pix;
            }
        }
    }
}

static void decode_proms(Mikie2* m) {
    for (int i = 0; i < 256; i++) {
        int bit0, bit1, bit2, bit3;
        bit0 = (m->proms[0x000 + i] >> 0) & 1;
        bit1 = (m->proms[0x000 + i] >> 1) & 1;
        bit2 = (m->proms[0x000 + i] >> 2) & 1;
        bit3 = (m->proms[0x000 + i] >> 3) & 1;
        int r = 0x0e*bit0 + 0x1f*bit1 + 0x43*bit2 + 0x8f*bit3;

        bit0 = (m->proms[0x100 + i] >> 0) & 1;
        bit1 = (m->proms[0x100 + i] >> 1) & 1;
        bit2 = (m->proms[0x100 + i] >> 2) & 1;
        bit3 = (m->proms[0x100 + i] >> 3) & 1;
        int g = 0x0e*bit0 + 0x1f*bit1 + 0x43*bit2 + 0x8f*bit3;

        bit0 = (m->proms[0x200 + i] >> 0) & 1;
        bit1 = (m->proms[0x200 + i] >> 1) & 1;
        bit2 = (m->proms[0x200 + i] >> 2) & 1;
        bit3 = (m->proms[0x200 + i] >> 3) & 1;
        int b = 0x0e*bit0 + 0x1f*bit1 + 0x43*bit2 + 0x8f*bit3;

        m->pal_argb[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    memcpy(m->chr_lut, &m->proms[0x300], 256);
    memcpy(m->spr_lut, &m->proms[0x400], 256);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Video render (fórmulas MAME)
 * ────────────────────────────────────────────────────────────────────────── */
static inline void putpix(Mikie2* m, int x, int y, uint32_t argb) {
    if ((unsigned)x < MIKIE_INT_W && (unsigned)y < MIKIE_INT_H)
        m->tmpbitmap[y * MIKIE_INT_W + x] = argb;
}

static void draw_tile(Mikie2* m, int code, int color, bool flipx, bool flipy, int sx, int sy) {
    const uint8_t (*src)[8] = m->char_pix[code & (MIKIE_NUM_CHARS-1)];
    for (int y = 0; y < 8; y++) {
        int yy = flipy ? (7 - y) : y;
        for (int x = 0; x < 8; x++) {
            int xx = flipx ? (7 - x) : x;
            uint8_t pen = src[yy][xx] & 0x0F;
            int idx = ((color & 0x0F) << 4) | pen;
            uint8_t prom = m->chr_lut[idx] & 0x0F;
            uint8_t pal = (uint8_t)(prom + 32 * (m->palettebank & 7) + 16);
            putpix(m, sx + x, sy + y, m->pal_argb[pal]);
        }
    }
}

static void draw_sprite(Mikie2* m, int gfxsel, int code, int color, bool flipx, bool flipy, int sx, int sy) {
    const uint8_t (*src)[16] = (gfxsel == 0) ? m->spr_pix0[code & 0xFF] : m->spr_pix1[code & 0xFF];
    for (int y = 0; y < 16; y++) {
        int yy = flipy ? (15 - y) : y;
        for (int x = 0; x < 16; x++) {
            int xx = flipx ? (15 - x) : x;
            uint8_t pen = src[yy][xx] & 0x0F;
            if (pen == 0) continue;
            int idx = ((color & 0x0F) << 4) | pen;
            uint8_t prom = m->spr_lut[idx] & 0x0F;
            uint8_t pal = (uint8_t)(prom + 32 * (m->palettebank & 7));
            putpix(m, sx + x, sy + y, m->pal_argb[pal]);
        }
    }
}

static void video_refresh(Mikie2* m) {
    for (int i = 0; i < MIKIE_INT_W * MIKIE_INT_H; i++)
        m->tmpbitmap[i] = 0xFF000000u;

    /* tiles */
    for (int offs = 0; offs < MIKIE_VRAM_SIZE; offs++) {
        int sx = offs % 32;
        int sy = offs / 32;
        bool flipx = (m->colorram[offs] & 0x40) != 0;
        bool flipy = (m->colorram[offs] & 0x80) != 0;
        int code = m->videoram[offs] + ((m->colorram[offs] & 0x20) << 3);
        int color = (m->colorram[offs] & 0x0F) + 16 * (m->palettebank & 7);

        if (m->flipscreen) {
            sx = 31 - sx;
            sy = 31 - sy;
            flipx = !flipx;
            flipy = !flipy;
        }

        draw_tile(m, code, color, flipx, flipy, 8*sx, 8*sy);
    }

    /* sprites */
    for (int offs = 0; offs < MIKIE_SPR_RAM_SIZE; offs += 4) {
        int sx = m->spriteram[offs + 3];
        int sy = 244 - m->spriteram[offs + 1];
        bool flipx = (~m->spriteram[offs] & 0x10) != 0;
        bool flipy = ( m->spriteram[offs] & 0x20) != 0;

        if (m->flipscreen) {
            sy = 242 - sy;
            flipy = !flipy;
        }

        int gfx = (m->spriteram[offs + 2] & 0x40) ? 1 : 0;
        int code = (m->spriteram[offs + 2] & 0x3f)
                 + ((m->spriteram[offs + 2] & 0x80) >> 1)
                 + ((m->spriteram[offs] & 0x40) << 1);
        int color = (m->spriteram[offs] & 0x0f) + 16 * (m->palettebank & 7);

        draw_sprite(m, gfx, code, color, flipx, flipy, sx, sy);
    }

    /* Rotación: para que no quede 180° invertida en tu SDL,
     * usamos la orientación opuesta a la primera versión.
     * Mapeo final (x,y) -> (rx,ry) = (255-(y+16), x)
     */
    for (int y = 0; y < MIKIE_SCREEN_H; y++) {
        int src_y = y + MIKIE_VIS_Y0;
        for (int x = 0; x < MIKIE_SCREEN_W; x++) {
            int rx = (MIKIE_INT_W - 1) - src_y;
            int ry = x;
            m->framebuffer[y*MIKIE_SCREEN_W + x] = m->tmpbitmap[ry*MIKIE_INT_W + rx];
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * CPU bus (mapa MAME)
 * ────────────────────────────────────────────────────────────────────────── */
static mc6809byte__t cpu_read(mc6809__t* cpu, mc6809addr__t addr, bool ifetch) {
    Mikie2* m = (Mikie2*)cpu->user;
    (void)ifetch;

    if (addr <= 0x00FF) return m->ram0[addr & 0xFF];

    switch (addr) {
    case 0x2400: return m->in0;
    case 0x2401: return m->in1;
    case 0x2402: return m->in2;
    case 0x2403: return m->in3;
    case 0x2500: return m->dsw0;
    case 0x2501: return m->dsw1;
    default: break;
    }

    if (addr >= 0x2800 && addr <= 0x2FFF) return m->ram2[addr - 0x2800];
    if (addr >= 0x3000 && addr <= 0x37FF) return m->ram3[addr - 0x3000];
    if (addr >= 0x3800 && addr <= 0x3BFF) return m->colorram[addr - 0x3800];
    if (addr >= 0x3C00 && addr <= 0x3FFF) return m->videoram[addr - 0x3C00];

    return m->rom[addr];
}

static void sound_trap_irq(Mikie2* m) {
    /* Trap simple: manda el latch al SN0 */
    sn_write(&m->sn[0], m->sound_latch);
}

static void cpu_write(mc6809__t* cpu, mc6809addr__t addr, mc6809byte__t val) {
    Mikie2* m = (Mikie2*)cpu->user;

    switch (addr) {
    case 0x2002: {
        uint8_t data = val & 1;
        if (m->last_irqtrig == 0 && data == 1)
            sound_trap_irq(m);
        m->last_irqtrig = data;
        return;
    }
    case 0x2006:
        m->flipscreen = (val & 1) != 0;
        return;
    case 0x2007:
        m->irq_enable = (val & 1) != 0;
        return;
    case 0x2100:
        return; /* watchdog */
    case 0x2200:
        m->palettebank = val & 7;
        return;
    case 0x2400:
        m->sound_latch = val;
        return;
    default:
        break;
    }

    if (addr <= 0x00FF) { m->ram0[addr & 0xFF] = val; return; }

    if (addr >= 0x2800 && addr <= 0x2FFF) {
        m->ram2[addr - 0x2800] = val;
        if (addr <= 0x288F) m->spriteram[addr - 0x2800] = val;
        return;
    }
    if (addr >= 0x3000 && addr <= 0x37FF) { m->ram3[addr - 0x3000] = val; return; }
    if (addr >= 0x3800 && addr <= 0x3BFF) { m->colorram[addr - 0x3800] = val; return; }
    if (addr >= 0x3C00 && addr <= 0x3FFF) { m->videoram[addr - 0x3C00] = val; return; }
}

static void cpu_fault(mc6809__t* cpu, mc6809fault__t fault) {
    fprintf(stderr, "[MC6809] Fault %d en PC=$%04X\n", fault, (unsigned)cpu->instpc);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Audio callback
 * ────────────────────────────────────────────────────────────────────────── */
static void audio_cb(void* userdata, Uint8* stream, int len) {
    Mikie2* m = (Mikie2*)userdata;
    float* out = (float*)stream;
    int samples = len / (int)sizeof(float);

    int t0 = (MIKIE_SN0_CLOCK / MIKIE_AUDIO_RATE) / 16;
    int t1 = (MIKIE_SN1_CLOCK / MIKIE_AUDIO_RATE) / 16;
    if (t0 < 1) t0 = 1;
    if (t1 < 1) t1 = 1;

    for (int i = 0; i < samples; i++) {
        float v = 0.0f;
        v += sn_sample(&m->sn[0], t0);
        v += sn_sample(&m->sn[1], t1);
        out[i] = v * 0.2f;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────────────────── */

void mikie2_init(Mikie2* m) {
    memset(m, 0, sizeof(*m));

    /* Inputs default */
    m->in0 = 0xFF;
    m->in1 = 0xFF;
    m->in2 = 0xFF;
    m->in3 = 0x00;
    m->dsw0 = 0xFF;
    m->dsw1 = 0x7B;

    m->irq_enable = true;
    m->palettebank = 0;
    m->flipscreen = false;

    sn_reset(&m->sn[0]);
    sn_reset(&m->sn[1]);

    /* Core mc6809: algunas variantes no tienen mc6809_init(). */
    memset(&m->cpu, 0, sizeof(m->cpu));
    m->cpu.read = cpu_read;
    m->cpu.write = cpu_write;
    m->cpu.fault = cpu_fault;
    m->cpu.user = m;
}

void mikie2_destroy(Mikie2* m) {
    if (m->audio_dev) SDL_CloseAudioDevice(m->audio_dev);
    if (m->texture) SDL_DestroyTexture(m->texture);
    if (m->renderer) SDL_DestroyRenderer(m->renderer);
    if (m->window) SDL_DestroyWindow(m->window);
    SDL_Quit();
}

int mikie2_load_roms(Mikie2* m, const char* dir) {
    int e = 0;
    memset(m->rom, 0xFF, sizeof(m->rom));

    /* CPU1 */
    e |= load_rom_exact(dir, "11c_n14.bin", m->rom, 0x6000, sizeof(m->rom), 0x2000);
    e |= load_rom_exact(dir, "12a_o13.bin", m->rom, 0x8000, sizeof(m->rom), 0x4000);
    e |= load_rom_exact(dir, "12d_o17.bin", m->rom, 0xC000, sizeof(m->rom), 0x4000);

    /* GFX */
    e |= load_rom_exact(dir, "o11", m->gfx1, 0x0000, sizeof(m->gfx1), 0x4000);
    e |= load_rom_exact(dir, "001", m->gfx2, 0x0000, sizeof(m->gfx2), 0x4000);
    e |= load_rom_exact(dir, "003", m->gfx2, 0x4000, sizeof(m->gfx2), 0x4000);
    e |= load_rom_exact(dir, "005", m->gfx2, 0x8000, sizeof(m->gfx2), 0x4000);
    e |= load_rom_exact(dir, "007", m->gfx2, 0xC000, sizeof(m->gfx2), 0x4000);

    /* PROMs */
    e |= load_rom_exact(dir, "01i_d19.bin", m->proms, 0x000, sizeof(m->proms), 0x100);
    e |= load_rom_exact(dir, "03i_d21.bin", m->proms, 0x100, sizeof(m->proms), 0x100);
    e |= load_rom_exact(dir, "02i_d20.bin", m->proms, 0x200, sizeof(m->proms), 0x100);
    e |= load_rom_exact(dir, "12h_d22.bin", m->proms, 0x300, sizeof(m->proms), 0x100);
    e |= load_rom_exact(dir, "f09_d18.bin", m->proms, 0x400, sizeof(m->proms), 0x100);

    if (e) return -1;

    decode_chars(m);
    decode_sprites_one(m, m->spr_pix0, 0);
    decode_sprites_one(m, m->spr_pix1, 1);
    decode_proms(m);

    printf("[MIKIE] ROMs OK en '%s'\n", dir);
    return 0;
}

void mikie2_run_frame(Mikie2* m) {
    int cycles = 0;
    int target = MIKIE_CYCLES_PER_FRAME;

    while (cycles < target) {
        unsigned long before = m->cpu.cycles;
        mc6809_step(&m->cpu);
        cycles += (int)(m->cpu.cycles - before);
    }

    if (m->irq_enable) {
        m->cpu.irq = true;
        mc6809_step(&m->cpu);
        m->cpu.irq = false;
    }

    video_refresh(m);
}

void mikie2_render(Mikie2* m) {
    SDL_UpdateTexture(m->texture, NULL, m->framebuffer, MIKIE_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(m->renderer);
    SDL_Rect dst = { 0, 0, MIKIE_SCREEN_W * MIKIE_SCALE, MIKIE_SCREEN_H * MIKIE_SCALE };
    SDL_RenderCopy(m->renderer, m->texture, NULL, &dst);
    SDL_RenderPresent(m->renderer);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Inputs
 * ────────────────────────────────────────────────────────────────────────── */
static void set_bit_low(uint8_t* reg, uint8_t mask, bool pressed) {
    if (pressed) *reg &= (uint8_t)~mask; else *reg |= mask;
}

static void handle_key(Mikie2* m, SDL_Scancode sc, bool dn) {
    switch (sc) {
    /* IN0 */
    case SDL_SCANCODE_5: set_bit_low(&m->in0, 0x01, dn); break;
    case SDL_SCANCODE_6: set_bit_low(&m->in0, 0x02, dn); break;
    case SDL_SCANCODE_7: set_bit_low(&m->in0, 0x04, dn); break;
    case SDL_SCANCODE_1: set_bit_low(&m->in0, 0x08, dn); break;
    case SDL_SCANCODE_2: set_bit_low(&m->in0, 0x10, dn); break;

    /* IN1 P1 */
    case SDL_SCANCODE_LEFT:  set_bit_low(&m->in1, 0x01, dn); break;
    case SDL_SCANCODE_RIGHT: set_bit_low(&m->in1, 0x02, dn); break;
    case SDL_SCANCODE_UP:    set_bit_low(&m->in1, 0x04, dn); break;
    case SDL_SCANCODE_DOWN:  set_bit_low(&m->in1, 0x08, dn); break;
    case SDL_SCANCODE_Z:     set_bit_low(&m->in1, 0x10, dn); break;
    case SDL_SCANCODE_X:     set_bit_low(&m->in1, 0x20, dn); break;

    /* IN2 P2 */
    case SDL_SCANCODE_A: set_bit_low(&m->in2, 0x01, dn); break;
    case SDL_SCANCODE_D: set_bit_low(&m->in2, 0x02, dn); break;
    case SDL_SCANCODE_W: set_bit_low(&m->in2, 0x04, dn); break;
    case SDL_SCANCODE_S: set_bit_low(&m->in2, 0x08, dn); break;
    case SDL_SCANCODE_Q: set_bit_low(&m->in2, 0x10, dn); break;
    case SDL_SCANCODE_E: set_bit_low(&m->in2, 0x20, dn); break;

    case SDL_SCANCODE_ESCAPE:
        if (dn) m->quit = true;
        break;
    case SDL_SCANCODE_F2:
        if (dn) m->turbo = !m->turbo;
        break;
    default:
        break;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * main
 * ────────────────────────────────────────────────────────────────────────── */
int main(int argc, char** argv) {
    const char* rom_dir = (argc > 1) ? argv[1] : ".";
    Mikie2* m = &mikie;

    mikie2_init(m);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    m->window = SDL_CreateWindow("Mikie (MAME-like)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                MIKIE_SCREEN_W * MIKIE_SCALE, MIKIE_SCREEN_H * MIKIE_SCALE, 0);
    m->renderer = SDL_CreateRenderer(m->window, -1, SDL_RENDERER_ACCELERATED);
    m->texture = SDL_CreateTexture(m->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   MIKIE_SCREEN_W, MIKIE_SCREEN_H);

    /* Audio mono float */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = MIKIE_AUDIO_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_cb;
    want.userdata = m;
    m->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (m->audio_dev) SDL_PauseAudioDevice(m->audio_dev, 0);

    if (mikie2_load_roms(m, rom_dir) != 0) {
        fprintf(stderr, "[MIKIE] Error cargando ROMs\n");
        mikie2_destroy(m);
        return 1;
    }

    mc6809_reset(&m->cpu);

    uint32_t last = SDL_GetTicks();
    while (!m->quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) m->quit = true;
            if (ev.type == SDL_KEYDOWN) handle_key(m, ev.key.keysym.scancode, true);
            if (ev.type == SDL_KEYUP)   handle_key(m, ev.key.keysym.scancode, false);
        }

        mikie2_run_frame(m);
        mikie2_render(m);

        if (!m->turbo) {
            uint32_t now = SDL_GetTicks();
            uint32_t frame_ms = 1000 / MIKIE_FPS;
            if (now - last < frame_ms) SDL_Delay(frame_ms - (now - last));
            last = SDL_GetTicks();
        }
    }

    mikie2_destroy(m);
    return 0;
}
