/*
 * snowbros.c  -  Emulador de Snow Bros. - Nick & Tom (Toaplan/Romstar, 1990)
 *
 * Estructura y estilo basados en zx.c / minivadr.c / ... / elevator.c
 * del proyecto zxtiny (chusogar/zxtiny).
 * Driver original: MAME 0.37b7 drivers/snowbros.c + vidhrdw/snowbros.c
 *                  (Mike Coates)
 *
 * CPU M68000: Musashi v3.4 (etchedpixels/EmulatorKit/m68k)
 *   - Instancia única (Musashi es single-instance por diseño)
 *   - Callbacks globales: cpu_read_byte/word/long, cpu_write_byte/word/long
 *   - IRQ autovectorizadas niveles 2, 3 y 4 (3 por frame)
 *   - cpu_irq_ack: devuelve M68K_INT_ACK_AUTOVECTOR
 *
 * CPU Z80: jgz80 (mismo core que el resto del proyecto)
 *   - NMI por soundlatch
 *   - IRQ causada por YM3812 (no emulado → sin IRQ de sonido)
 *
 * GFX: 4096 tiles 16x16 4bpp, decodificados en heap
 *   tilelayout: { 0,1,2,3 } planes packed,
 *   pixels cols 0-7: STEP8(0,4), cols 8-15: STEP8(8*32,4)
 *   rows  0-7: STEP8(0,32), rows 8-15: STEP8(16*32,32)
 *   stride: 32*32 = 1024 bits = 128 bytes por tile
 *
 * Paleta: paletteram_xBBBBBGGGGGRRRRR_word (16-bit big-endian)
 *   R = (val & 0x001F) << 3
 *   G = ((val >> 5)  & 0x1F) << 3
 *   B = ((val >> 10) & 0x1F) << 3
 */

#include "snowbros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Variable global para callbacks de Musashi
// ---------------------------------------------------------------------------
SnowBros* g_sb = NULL;

// ---------------------------------------------------------------------------
// Callbacks requeridos por m68kconf.h de EmulatorKit
// ---------------------------------------------------------------------------

void cpu_pulse_reset(void) { /* RESET instruction: ignorar */ }
void cpu_instr_callback(void) { /* hook por instrucción: no usado */ }
void cpu_set_fc(int fc) { (void)fc; /* function code: no usado */ }

int cpu_irq_ack(int level) {
    (void)level;
    /* Autovector: el hardware de Snow Bros usa autovectores */
    return M68K_INT_ACK_AUTOVECTOR;
}

// ---------------------------------------------------------------------------
// Lectura de memoria para Musashi (big-endian → correcto para 68000)
// ---------------------------------------------------------------------------

static inline uint16_t sb_read16(uint32_t addr) {
    SnowBros* sb = g_sb;

    // ROM 0x000000-0x03FFFF
    if (addr <= 0x03FFFE) {
        return ((uint16_t)sb->rom[addr] << 8) | sb->rom[addr + 1];
    }

    // RAM 0x100000-0x103FFF
    if (addr >= 0x100000 && addr <= 0x103FFE) {
        int off = addr - 0x100000;
        return ((uint16_t)sb->ram[off] << 8) | sb->ram[off + 1];
    }

    // Sound status / soundlatch read 0x300000
    if (addr == 0x300000) {
        return (uint16_t)sb->soundlatch;
    }

    // Input 0x500000-0x500005
    if (addr >= 0x500000 && addr <= 0x500004) {
        int off = addr - 0x500000;
        switch (off) {
        case 0: return ((uint16_t)sb->dsw[0] << 8) | sb->in[0];  // DSW1 | P1
        case 2: return ((uint16_t)sb->dsw[1] << 8) | sb->in[1];  // DSW2 | P2
        case 4: return (uint16_t)(sb->in[2] << 8);                // coins/start
        }
    }

    // Palette RAM 0x600000-0x6001FF
    if (addr >= 0x600000 && addr <= 0x6001FE) {
        int off = addr - 0x600000;
        return ((uint16_t)sb->palram[off] << 8) | sb->palram[off + 1];
    }

    // Sprite RAM 0x700000-0x701DFE
    if (addr >= 0x700000 && addr <= 0x701DFE) {
        int off = addr - 0x700000;
        return ((uint16_t)sb->spriteram[off] << 8) | sb->spriteram[off + 1];
    }

    return 0xFFFF;
}

static inline void sb_write16(uint32_t addr, uint16_t val) {
    SnowBros* sb = g_sb;

    // ROM: solo lectura
    if (addr <= 0x03FFFF) return;

    // RAM
    if (addr >= 0x100000 && addr <= 0x103FFE) {
        int off = addr - 0x100000;
        sb->ram[off]     = (uint8_t)(val >> 8);
        sb->ram[off + 1] = (uint8_t)(val & 0xFF);
        return;
    }

    // Sound command 0x300000
    if (addr == 0x300000) {
        sb->soundlatch = (uint8_t)(val & 0xFF);
        sb->snd_nmi_pending = true;
        return;
    }

    // IRQ ACK 0x800000, 0x900000, 0xA00000: NOP
    if (addr == 0x800000 || addr == 0x900000 || addr == 0xA00000) {
        return;
    }

    // Watchdog 0x200000: NOP
    if (addr == 0x200000) return;

    // Palette RAM 0x600000-0x6001FF
    if (addr >= 0x600000 && addr <= 0x6001FE) {
        int off = addr - 0x600000;
        sb->palram[off]     = (uint8_t)(val >> 8);
        sb->palram[off + 1] = (uint8_t)(val & 0xFF);
        // Actualizar paleta ARGB inmediatamente
        int color_idx = off / 2;
        uint16_t c = val;
        uint8_t r = (uint8_t)((c & 0x001F) << 3);
        uint8_t g = (uint8_t)(((c >> 5) & 0x1F) << 3);
        uint8_t b = (uint8_t)(((c >> 10) & 0x1F) << 3);
        sb->palette[color_idx] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
        return;
    }

    // Sprite RAM 0x700000-0x701DFE
    if (addr >= 0x700000 && addr <= 0x701DFE) {
        int off = addr - 0x700000;
        sb->spriteram[off]     = (uint8_t)(val >> 8);
        sb->spriteram[off + 1] = (uint8_t)(val & 0xFF);
        return;
    }
}

// ---------------------------------------------------------------------------
// Funciones de acceso a memoria requeridas por m68kconf.h
// (nombres exactos definidos en m68kconf.h del EmulatorKit)
// ---------------------------------------------------------------------------

unsigned int cpu_read_byte(unsigned int addr) {
    uint16_t w = sb_read16(addr & ~1);
    return (addr & 1) ? (w & 0xFF) : (w >> 8);
}

unsigned int cpu_read_word(unsigned int addr) {
    return sb_read16(addr & ~1);
}

unsigned int cpu_read_long(unsigned int addr) {
    return ((uint32_t)sb_read16(addr & ~1) << 16) | sb_read16((addr + 2) & ~1);
}

unsigned int cpu_read_word_dasm(unsigned int addr) { return cpu_read_word(addr); }
unsigned int cpu_read_long_dasm(unsigned int addr) { return cpu_read_long(addr); }

void cpu_write_byte(unsigned int addr, unsigned int val) {
    uint16_t w = sb_read16(addr & ~1);
    if (addr & 1) w = (w & 0xFF00) | (val & 0xFF);
    else          w = (w & 0x00FF) | ((val & 0xFF) << 8);
    sb_write16(addr & ~1, w);
}

void cpu_write_word(unsigned int addr, unsigned int val) {
    sb_write16(addr & ~1, (uint16_t)val);
}

void cpu_write_long(unsigned int addr, unsigned int val) {
    sb_write16(addr & ~1,       (uint16_t)(val >> 16));
    sb_write16((addr + 2) & ~1, (uint16_t)(val & 0xFFFF));
}

void cpu_write_long_pd(unsigned int addr, unsigned int val) {
    /* predecrement: escribir high word primero */
    sb_write16((addr + 2) & ~1, (uint16_t)(val & 0xFFFF));
    sb_write16(addr & ~1,       (uint16_t)(val >> 16));
}

// ---------------------------------------------------------------------------
// Callbacks Z80 (sound CPU)
// ---------------------------------------------------------------------------

static uint8_t snd_read(void* ud, uint16_t addr) {
    SnowBros* sb = (SnowBros*)ud;
    if (addr <= 0x7FFF) return sb->sndrom[addr];
    if (addr >= 0x8000 && addr <= 0x87FF) return sb->sndram[addr - 0x8000];
    return 0xFF;
}

static void snd_write(void* ud, uint16_t addr, uint8_t val) {
    SnowBros* sb = (SnowBros*)ud;
    if (addr >= 0x8000 && addr <= 0x87FF) { sb->sndram[addr - 0x8000] = val; return; }
}

static uint8_t snd_port_in(z80* z, uint16_t port) {
    SnowBros* sb = (SnowBros*)z->userdata;
    uint8_t p = (uint8_t)(port & 0xFF);
    if (p == 0x02) return 0x00;   // YM3812 status: no busy
    if (p == 0x04) return sb->soundlatch;
    return 0xFF;
}

static void snd_port_out(z80* z, uint16_t port, uint8_t val) {
    SnowBros* sb = (SnowBros*)z->userdata;
    uint8_t p = (uint8_t)(port & 0xFF);
    if (p == 0x04) { /* soundlatch back to main: ignorado */ (void)sb; }
    // YM3812: ignorado (sin síntesis de audio)
    (void)val;
}

// ---------------------------------------------------------------------------
// Utilidades de carga
// ---------------------------------------------------------------------------

static int load_file(uint8_t* dst, int max_size, const char* path,
                     int offset, int size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    int to_read = (size > 0) ? size : (int)fsz;
    if (offset + to_read > max_size) {
        fprintf(stderr, "[ROM] '%s': desbordamiento\n", path); fclose(f); return -1;
    }
    int n = (int)fread(dst + offset, 1, (size_t)to_read, f);
    fclose(f);
    if (n != to_read) {
        fprintf(stderr, "[ROM] '%s': leidos %d de %d\n", path, n, to_read); return -1;
    }
    printf("[ROM] '%s' (%d bytes)\n", path, n);
    return 0;
}

// ROM entrelazada de 16 bits: los bytes pares van a posiciones pares de rom[]
// y los bytes impares a posiciones impares.
// ROM_LOAD_EVEN: cada byte del fichero va a rom[i*2]
// ROM_LOAD_ODD:  cada byte del fichero va a rom[i*2+1]

int snowbros_load_rom_even(SnowBros* sb, const char* path) {
    uint8_t tmp[SB_ROM_SIZE / 2];
    int r = load_file(tmp, SB_ROM_SIZE / 2, path, 0, 0);
    if (r != 0) return r;
    int n = SB_ROM_SIZE / 2;
    for (int i = 0; i < n; i++) sb->rom[i * 2] = tmp[i];
    sb->have_rom = true;
    return 0;
}

int snowbros_load_rom_odd(SnowBros* sb, const char* path) {
    uint8_t tmp[SB_ROM_SIZE / 2];
    int r = load_file(tmp, SB_ROM_SIZE / 2, path, 0, 0);
    if (r != 0) return r;
    int n = SB_ROM_SIZE / 2;
    for (int i = 0; i < n; i++) sb->rom[i * 2 + 1] = tmp[i];
    return 0;
}

int snowbros_load_sndrom(SnowBros* sb, const char* path) {
    int r = load_file(sb->sndrom, SB_SNDROM_SIZE, path, 0, 0);
    if (r == 0) sb->have_sndrom = true;
    return r;
}

int snowbros_load_gfx(SnowBros* sb, const char* path, int offset, int size) {
    int r = load_file(sb->gfx, SB_GFX_SIZE, path, offset, size);
    if (r == 0) sb->have_gfx = true;
    return r;
}

// ---------------------------------------------------------------------------
// Decodificación de GFX
//
// tilelayout Snow Bros (MAME):
//   16x16, 4bpp
//   planes: { 0, 1, 2, 3 }  ← 4 bits consecutivos por pixel
//   pixels cols 0-7:  STEP8(0, 4)        = bit_offsets 0,4,8,12,16,20,24,28
//   pixels cols 8-15: STEP8(8*32, 4)     = bit_offsets 256,260,...284
//   rows 0-7:   STEP8(0, 32)             = byte_offsets 0,4,8,12,...28
//   rows 8-15:  STEP8(16*32, 32)         = byte_offsets 512,516,...540  (en bits: ×32)
//   stride: 32*32 bits = 128 bytes
//
// Para tile t, fila row, columna col:
//   row_group = row / 8          (0=filas 0-7, 1=filas 8-15)
//   col_group = col / 8          (0=cols 0-7, 1=cols 8-15)
//   row_in_group = row % 8
//   col_in_group = col % 8
//   bit_offset = row_group * 16*32 + row_in_group * 32
//              + col_group * 8*32  + col_in_group * 4
//   byte_off_in_tile = bit_offset / 8
//   nibble_shift = (bit_offset % 8 == 0) ? 4 : 0   (bit 0 del pixel es bit4 del byte)
//   pixel nibble = (gfx[tile*128 + byte_off_in_tile] >> nibble_shift) & 0x0F
// ---------------------------------------------------------------------------

void snowbros_decode_gfx(SnowBros* sb) {
    if (!sb->tile_pix || !sb->have_gfx) return;
    for (int t = 0; t < SB_TILES_NUM; t++) {
        for (int row = 0; row < SB_TILE_H; row++) {
            int rg = row / 8, ri = row % 8;
            for (int col = 0; col < SB_TILE_W; col++) {
                int cg = col / 8, ci = col % 8;
                // bit offset dentro del tile
                int bit_off = rg * (16 * 32) + ri * 32
                            + cg * (8 * 32)  + ci * 4;
                int byte_off = t * SB_TILE_STRIDE + bit_off / 8;
                if (byte_off >= SB_GFX_SIZE) continue;
                uint8_t b = sb->gfx[byte_off];
                // Los 4 bits del pixel están en bit_off%8..bit_off%8+3
                // bit_off es siempre múltiplo de 4, así que bit_off%8 = 0 o 4
                int shift = (bit_off & 4) ? 0 : 4;
                sb->tile_pix[t][row][col] = (b >> shift) & 0x0F;
            }
        }
    }
    printf("[GFX] %d tiles decodificados\n", SB_TILES_NUM);
}

// ---------------------------------------------------------------------------
// snowbros_init
// ---------------------------------------------------------------------------

void snowbros_init(SnowBros* sb) {
    memset(sb, 0, sizeof(*sb));
    g_sb = sb;

    // Heap para tiles
    sb->tile_pix = calloc(SB_TILES_NUM, sizeof(*sb->tile_pix));

    // Input: active-low (sin pulsar = 0xFF)
    sb->in[0] = 0xFF;
    sb->in[1] = 0xFF;
    sb->in[2] = 0xFF;
    // bit7 de in[0] e in[1] debe estar a 0 (active-high, probablemente VBLANK)
    // El comentario de MAME dice "Must be low or game stops! probably VBlank"
    // El bit es IP_ACTIVE_HIGH en el driver, así que 0 = no VBLANK.
    // Lo gestionamos forzando ese bit a 0 siempre.
    sb->in[0] &= ~0x80;
    sb->in[1] &= ~0x80;

    // DSW: valores por defecto (juego en modo normal)
    sb->dsw[0] = 0xFF;  // America, Flip off, Demo sounds on, 1C/1C x2
    sb->dsw[1] = 0xFF;  // Normal difficulty, 100k bonus, 3 lives, etc.

    // Paleta inicial negra
    memset(sb->palette, 0, sizeof(sb->palette));

    // M68000 (Musashi)
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_pulse_reset();

    // Z80 de sonido
    z80_init(&sb->cpu_snd);
    sb->cpu_snd.userdata   = sb;
    sb->cpu_snd.read_byte  = snd_read;
    sb->cpu_snd.write_byte = snd_write;
    sb->cpu_snd.port_in    = snd_port_in;
    sb->cpu_snd.port_out   = snd_port_out;

    // SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    sb->window = SDL_CreateWindow(
        "Snow Bros. - Nick & Tom (Toaplan, 1990)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SB_SCREEN_W * SB_SCALE, SB_SCREEN_H * SB_SCALE,
        SDL_WINDOW_SHOWN);
    sb->renderer = SDL_CreateRenderer(sb->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    sb->texture = SDL_CreateTexture(sb->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, SB_SCREEN_W, SB_SCREEN_H);
    SDL_RenderSetLogicalSize(sb->renderer,
        SB_SCREEN_W * SB_SCALE, SB_SCREEN_H * SB_SCALE);
}

void snowbros_destroy(SnowBros* sb) {
    free(sb->tile_pix);
    if (sb->texture)  SDL_DestroyTexture(sb->texture);
    if (sb->renderer) SDL_DestroyRenderer(sb->renderer);
    if (sb->window)   SDL_DestroyWindow(sb->window);
    SDL_Quit();
    g_sb = NULL;
}

// ---------------------------------------------------------------------------
// Renderizado: snowbros_vh_screenrefresh
//
// MAME itera sobre offs=0..0x1DFF en pasos de 16:
//   sx raw = spriteram[8+offs] (byte bajo del word)
//   sy raw = spriteram[0xA+offs] (byte bajo del word)
//   tilecolour = spriteram[6+offs] word (big-endian)
//   Si tilecolour & 1: sx = -1-(sx^0xFF) (complemento con signo)
//   Si tilecolour & 2: sy = -1-(sy^0xFF)
//   Si tilecolour & 4: x+=sx, y+=sy (offset relativo)
//   else: x=sx, y=sy
//   Si x>511: x &= 0x1FF
//   Si y>511: y &= 0x1FF
//   Visible: x>-16 && y>0 && x<256 && y<240
//   tile = ((attr&0xF)<<8) | code_lo   (attr=word@0xE, code_lo=byte@0xD)
//   palette = (tilecolour & 0xF0) >> 4
//   flipY = attr & 0x80
//   flipX = attr & 0x40
// ---------------------------------------------------------------------------

void snowbros_render(SnowBros* sb) {
    // Limpiar con color de fondo (palette[0])
    uint32_t bg = sb->palette[0];
    for (int i = 0; i < SB_SCREEN_W * SB_SCREEN_H; i++)
        sb->framebuffer[i] = bg;

    if (!sb->tile_pix) goto present;

    {
        int x = 0, y = 0;
        for (int offs = 0; offs < (int)SB_SPRITERAM_SIZE; offs += 16) {
            // Leer words en big-endian desde spriteram
            uint16_t tc_word = ((uint16_t)sb->spriteram[6  + offs] << 8) |
                                           sb->spriteram[7  + offs];
            uint8_t  sx_raw  =             sb->spriteram[9  + offs]; // byte bajo del word en offs+8
            uint8_t  sy_raw  =             sb->spriteram[11 + offs]; // byte bajo del word en offs+A
            uint8_t  code_lo =             sb->spriteram[13 + offs]; // byte bajo del word en offs+C
            uint16_t attr    = ((uint16_t)sb->spriteram[14 + offs] << 8) |
                                           sb->spriteram[15 + offs];

            int tilecolour = tc_word;
            int sx = (int)sx_raw;
            int sy = (int)sy_raw;

            // Signo
            if (tilecolour & 1) sx = -1 - (sx ^ 0xFF);
            if (tilecolour & 2) sy = -1 - (sy ^ 0xFF);

            // Offset relativo o absoluto
            if (tilecolour & 4) { x += sx; y += sy; }
            else                { x  = sx; y  = sy; }

            if (x > 511) x &= 0x1FF;
            if (y > 511) y &= 0x1FF;

            // Visible
            if (x <= -16 || y <= 0 || x >= 256 || y >= 240) continue;

            int tile    = ((attr & 0x0F) << 8) | code_lo;
            int palette = (tilecolour >> 4) & 0x0F;
            bool flipy  = (attr & 0x0080) != 0;
            bool flipx  = (attr & 0x0040) != 0;

            if (tile >= SB_TILES_NUM) continue;

            // Dibujar tile 16x16 en framebuffer con recorte a [VIS_Y0, VIS_Y1]
            for (int row = 0; row < SB_TILE_H; row++) {
                int py = y + (flipy ? SB_TILE_H - 1 - row : row);
                if (py < SB_VIS_Y0 || py > SB_VIS_Y1) continue;
                int fy = py - SB_VIS_Y0;
                for (int col = 0; col < SB_TILE_W; col++) {
                    int px = x + (flipx ? SB_TILE_W - 1 - col : col);
                    if (px < 0 || px >= SB_SCREEN_W) continue;
                    uint8_t pix = sb->tile_pix[tile][row][col];
                    if (pix == SB_TRANSPARENT_PEN) continue;
                    int color_idx = palette * 16 + pix;
                    sb->framebuffer[fy * SB_SCREEN_W + px] = sb->palette[color_idx];
                }
            }
        }
    }

present:
    SDL_UpdateTexture(sb->texture, NULL, sb->framebuffer,
                      SB_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(sb->renderer);
    SDL_Rect dst = { 0, 0, SB_SCREEN_W * SB_SCALE, SB_SCREEN_H * SB_SCALE };
    SDL_RenderCopy(sb->renderer, sb->texture, NULL, &dst);
    SDL_RenderPresent(sb->renderer);
}

// ---------------------------------------------------------------------------
// snowbros_run_frame
//
// MAME: interrupt_3 = lanza IRQ niveles 4, 3, 2 (3 veces por frame)
// "cpu_getiloops() + 2" devuelve 2, 3, 4 en las 3 iteraciones.
// En Musashi: m68k_set_irq(level) activa la IRQ; después de ser servida
// se limpia automáticamente con autovector.
// Implementamos: ejecutar 1/3 del frame, lanzar IRQ4; 1/3, IRQ3; 1/3, IRQ2.
// ---------------------------------------------------------------------------

void snowbros_run_frame(SnowBros* sb) {
    const int slice = SB_CYCLES_PER_FRAME / SB_IRQ_SLICES;

    // Actualizar bit7 de IN0/IN1: 0 siempre (no VBLANK fuera de período)
    sb->in[0] &= ~0x80;
    sb->in[1] &= ~0x80;

    for (int i = 0; i < SB_IRQ_SLICES; i++) {
        m68k_execute(slice);

        // IRQ autovectorizadas: 4, 3, 2 (iloops + 2)
        int irq_level = 4 - i;   // 4, 3, 2
        m68k_set_irq(irq_level);
        // El 68000 las atiende automáticamente en el siguiente execute;
        // limpiamos inmediatamente (autovector = IRQ puntual)
        m68k_set_irq(M68K_IRQ_NONE);
    }

    // Sound CPU
    if (sb->snd_nmi_pending) {
        sb->snd_nmi_pending = false;
        z80_pulse_nmi(&sb->cpu_snd);
    }
    z80_step_n(&sb->cpu_snd, SB_SND_CYCLES_PER_FRAME);

    sb->frame_counter++;
}

// ---------------------------------------------------------------------------
// snowbros_handle_key
// IN0 (0x500001): P1 joystick + buttons, active-low
//   bit0=UP, bit1=DOWN, bit2=LEFT, bit3=RIGHT, bit4=BTN1, bit5=BTN2, bit6=BTN3
//   bit7=VBLANK (active-high, SIEMPRE 0)
// IN1 (0x500003): P2 (mismo mapa)
// IN2 (0x500005, byte alto):
//   bit0=START1, bit1=START2, bit2=COIN1, bit3=COIN2, bit5=TILT, bit6=COIN3
// ---------------------------------------------------------------------------

void snowbros_handle_key(SnowBros* sb, SDL_Scancode sc, bool pressed) {
    uint8_t* reg = NULL;
    uint8_t  bit = 0;

    switch (sc) {
    // P1 joystick
    case SDL_SCANCODE_UP:    reg = &sb->in[0]; bit = 0x01; break;
    case SDL_SCANCODE_DOWN:  reg = &sb->in[0]; bit = 0x02; break;
    case SDL_SCANCODE_LEFT:  reg = &sb->in[0]; bit = 0x04; break;
    case SDL_SCANCODE_RIGHT: reg = &sb->in[0]; bit = 0x08; break;
    // P1 buttons
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_Z:     reg = &sb->in[0]; bit = 0x10; break;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_X:     reg = &sb->in[0]; bit = 0x20; break;
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_C:     reg = &sb->in[0]; bit = 0x40; break;
    // Start / Coin
    case SDL_SCANCODE_1:     reg = &sb->in[2]; bit = 0x01; break;  // START1
    case SDL_SCANCODE_2:     reg = &sb->in[2]; bit = 0x02; break;  // START2
    case SDL_SCANCODE_5:     reg = &sb->in[2]; bit = 0x04; break;  // COIN1
    case SDL_SCANCODE_6:     reg = &sb->in[2]; bit = 0x08; break;  // COIN2
    case SDL_SCANCODE_ESCAPE:
        if (pressed) { sb->quit = true; } return;
    default: return;
    }

    if (reg) {
        if (pressed) *reg &= ~bit;  // active-low
        else         *reg |=  bit;
    }
    // Mantener bit7 a 0
    sb->in[0] &= ~0x80;
    sb->in[1] &= ~0x80;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void print_usage(const char* exe) {
    printf("Uso: %s [opciones]\n\n", exe);
    printf("Emulador de Snow Bros. - Nick & Tom (Toaplan/Romstar, 1990)\n\n");
    printf("ROMs de programa (68000, entrelazadas 16-bit):\n");
    printf("  --even <f>   sn6.bin (bytes pares, 128KB)\n");
    printf("  --odd  <f>   sn5.bin (bytes impares, 128KB)\n");
    printf("ROM de sonido (Z80):\n");
    printf("  --snd  <f>   snowbros.4 (32KB)\n");
    printf("GFX ROMs (4 chips de 128KB):\n");
    printf("  --ch0  <f>   ch0 (0x00000, 128KB)\n");
    printf("  --ch1  <f>   ch1 (0x20000, 128KB)\n");
    printf("  --ch2  <f>   ch2 (0x40000, 128KB)\n");
    printf("  --ch3  <f>   ch3 (0x60000, 128KB)\n\n");
    printf("Controles:\n");
    printf("  Flechas       Joystick P1\n");
    printf("  Ctrl/Z        Botón 1 (lanzar bola de nieve)\n");
    printf("  Alt/X         Botón 2\n");
    printf("  Espacio/C     Botón 3\n");
    printf("  1 / 2         Start P1 / P2\n");
    printf("  5 / 6         Coin 1 / Coin 2\n");
    printf("  Escape        Salir\n");
}

int main(int argc, char* argv[]) {
    static SnowBros sb;
    snowbros_init(&sb);

    const char *f_even=NULL, *f_odd=NULL, *f_snd=NULL;
    const char *f_ch[4] = {NULL,NULL,NULL,NULL};

    for (int i = 1; i < argc; i++) {
#define ARG(f,v) if(!strcmp(argv[i],f)&&i+1<argc){v=argv[++i];continue;}
        ARG("--even", f_even)
        ARG("--odd",  f_odd)
        ARG("--snd",  f_snd)
        ARG("--ch0",  f_ch[0])
        ARG("--ch1",  f_ch[1])
        ARG("--ch2",  f_ch[2])
        ARG("--ch3",  f_ch[3])
        if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) {
            print_usage(argv[0]); snowbros_destroy(&sb); return 0;
        }
#undef ARG
    }

    if (argc < 2) { print_usage(argv[0]); snowbros_destroy(&sb); return 0; }

    // Cargar ROMs
    if (f_even) snowbros_load_rom_even(&sb, f_even);
    if (f_odd)  snowbros_load_rom_odd(&sb, f_odd);
    if (f_snd)  snowbros_load_sndrom(&sb, f_snd);

    const int gfx_offs[4] = { 0x00000, 0x20000, 0x40000, 0x60000 };
    for (int i = 0; i < 4; i++)
        if (f_ch[i]) snowbros_load_gfx(&sb, f_ch[i], gfx_offs[i], 0x20000);

    // Decodificar GFX y reiniciar M68000 con ROM cargada
    snowbros_decode_gfx(&sb);
    m68k_pulse_reset();  // reset con ROM ya cargada

    if (!sb.have_rom)
        fprintf(stderr, "Aviso: ROM principal no cargada.\n");

    const uint32_t FRAME_MS = 1000 / SB_FPS;
    printf("Snow Bros. - iniciando emulacion. ESC para salir.\n");

    while (!sb.quit) {
        uint32_t t0 = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) sb.quit = true;
            else if (e.type == SDL_KEYDOWN) snowbros_handle_key(&sb, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)   snowbros_handle_key(&sb, e.key.keysym.scancode, false);
        }
        snowbros_run_frame(&sb);
        if (sb.turbo_mode) {
            if ((sb.frame_counter & 7) == 0) snowbros_render(&sb);
        } else {
            snowbros_render(&sb);
            uint32_t el = SDL_GetTicks() - t0;
            if (el < FRAME_MS) SDL_Delay(FRAME_MS - el);
        }
    }

    snowbros_destroy(&sb);
    return 0;
}
