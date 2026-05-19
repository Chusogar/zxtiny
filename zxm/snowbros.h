#ifndef SNOWBROS_H
#define SNOWBROS_H

/*
 * snowbros.h  -  Emulador de Snow Bros. - Nick & Tom (Toaplan/Romstar, 1990)
 *
 * Hardware:
 *   CPU main  : Motorola 68000 @ 8 MHz
 *               IRQs autovectorizados: 2, 3 y 4 (3 por frame via cpu_getiloops)
 *   CPU sound : Z80 @ 3.6 MHz, IRQ causada por YM3812 (ignorada aqui), NMI por soundlatch
 *
 *   ROM main  : 0x000000-0x03FFFF (256 KB)
 *                 sn6.bin (even bytes) + sn5.bin (odd bytes) → ROM entrelazada 16-bit
 *
 *   ROM sound : 0x0000-0x7FFF (32 KB)  snowbros.4
 *
 *   RAM main  : 0x100000-0x103FFF (16 KB)
 *
 *   Paleta    : 0x600000-0x6001FF (256 entradas x 2 bytes = 512 bytes)
 *               formato xBBBBBGGGGGRRRRR (word big-endian)
 *               R = (val & 0x001F) * 8
 *               G = ((val >> 5) & 0x001F) * 8
 *               B = ((val >> 10) & 0x001F) * 8
 *
 *   Sprite RAM: 0x700000-0x701DFF (0x1E00 bytes = 1920 bytes, 120 sprites x 16 bytes)
 *
 *   Sound latch: 0x300000 (main escribe, sound lee)
 *   Input:       0x500000-0x500005 (3 words)
 *
 *   Sprites: 16x16 tiles 4bpp, 16 bancos de color, hasta 120 sprites activos.
 *            Formato sprite RAM (16 bytes por sprite, MAME vidhrdw):
 *              offset 6  (word): tilecolour
 *                bits 7-4: palette bank (0-15)
 *                bit  0:   sx sign bit
 *                bit  1:   sy sign bit
 *                bit  2:   use relative offsets
 *              offset 8  (word lo byte): sx raw
 *              offset 0xA (word lo byte): sy raw
 *              offset 0xC (word lo byte): sprite number low 8 bits
 *              offset 0xE (word):
 *                bits 3-0: sprite number high 4 bits → tile = (attr&0xF)<<8 | code_lo
 *                bit  7:   flipY
 *                bit  6:   flipX
 *
 *   GFX layout (tilelayout):
 *     16x16, 4bpp packed
 *     planes: { 0, 1, 2, 3 }  (4 bits consecutivos por pixel)
 *     pixels cols 0-7:  { STEP8(0,4) }       = bits 0-3, 4-7, 8-11, ... 28-31
 *     pixels cols 8-15: { STEP8(8*32,4) }    = bits 256+0..256+28
 *     rows 0-7:  { STEP8(0,32) }             = byte 0, 4, 8, ...
 *     rows 8-15: { STEP8(16*32,32) }         = byte 512+...
 *     stride: 32*32 bits = 128 bytes por tile
 *     Total tiles: 0x80000 / 128 = 4096 tiles
 *
 *   Pantalla: 256x256 logico, visible { 0,255, 16,239 } = 256x224, ROT0
 *
 *   Palette: VIDEO_MODIFIES_PALETTE (paletteram en RAM, 256 colores de 16 bits)
 *            colortable GFX[0]: base 0, 16 grupos x 16 colores = 256 total
 *            transparente: pen 0
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

/* Musashi M68000 core */
#include "m68k/m68k.h"

/* Z80 core del proyecto */
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Mapa de memoria
// ---------------------------------------------------------------------------
#define SB_ROM_SIZE         0x040000  // 256 KB
#define SB_RAM_SIZE         0x004000  // 16 KB  (0x100000-0x103FFF)
#define SB_PALRAM_SIZE      0x000200  // 512 bytes = 256 colores x 2 (0x600000-0x6001FF)
#define SB_SPRITERAM_SIZE   0x001E00  // 0x1E00 bytes (0x700000-0x701DFF)
#define SB_SNDROM_SIZE      0x008000  // 32 KB
#define SB_SNDRAM_SIZE      0x000800  // 2 KB  (0x8000-0x87FF)

// ---------------------------------------------------------------------------
// GFX
// ---------------------------------------------------------------------------
#define SB_GFX_SIZE         0x080000  // 512 KB (4 chips x 128 KB)
#define SB_TILES_NUM        4096      // 0x80000 / 128 bytes por tile
#define SB_TILE_W           16
#define SB_TILE_H           16
#define SB_TILE_STRIDE      128       // bytes por tile en ROM

// ---------------------------------------------------------------------------
// Paleta
// ---------------------------------------------------------------------------
#define SB_PALETTE_COLORS   256       // 16 bancos x 16 colores
#define SB_TRANSPARENT_PEN  0         // pen 0 = transparente

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
#define SB_SCREEN_W         256
#define SB_SCREEN_H         224       // visible_area y=[16,239]
#define SB_LOG_W            256
#define SB_LOG_H            256
#define SB_VIS_Y0           16
#define SB_VIS_Y1           239
#define SB_SCALE            2

// ---------------------------------------------------------------------------
// Temporización
// ---------------------------------------------------------------------------
#define SB_CPU_CLOCK        8000000   // 8 MHz
#define SB_SND_CLOCK        3600000   // 3.6 MHz
#define SB_FPS              60
#define SB_CYCLES_PER_FRAME (SB_CPU_CLOCK / SB_FPS)      // 133333
#define SB_SND_CYCLES_PER_FRAME (SB_SND_CLOCK / SB_FPS)  // 60000

// IRQs: 3 por frame (niveles 2, 3, 4) en ciclos equidistantes
#define SB_IRQ_SLICES       3

// ---------------------------------------------------------------------------
// Estructura principal
// ---------------------------------------------------------------------------
typedef struct {
    /* ── Memoria ─────────────────────────────────────────────────── */
    uint8_t  rom[SB_ROM_SIZE];          // 0x000000-0x03FFFF
    uint8_t  ram[SB_RAM_SIZE];          // 0x100000-0x103FFF
    uint8_t  palram[SB_PALRAM_SIZE];    // 0x600000-0x6001FF
    uint8_t  spriteram[SB_SPRITERAM_SIZE]; // 0x700000-0x701DFF
    uint8_t  sndrom[SB_SNDROM_SIZE];
    uint8_t  sndram[SB_SNDRAM_SIZE];

    /* ── GFX ─────────────────────────────────────────────────────── */
    uint8_t  gfx[SB_GFX_SIZE];         // GFX ROMs crudas
    // Tiles decodificados: [tile][row][col] = nibble 0-15
    uint8_t  (*tile_pix)[SB_TILE_H][SB_TILE_W];

    /* ── Paleta ARGB ─────────────────────────────────────────────── */
    uint32_t palette[SB_PALETTE_COLORS];

    /* ── Video ───────────────────────────────────────────────────── */
    uint32_t framebuffer[SB_SCREEN_W * SB_SCREEN_H];

    /* ── SDL ─────────────────────────────────────────────────────── */
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;

    /* ── CPUs ────────────────────────────────────────────────────── */
    // M68000: gestionado globalmente por Musashi (instancia única)
    // M68000 context guardado/restaurado con m68k_get_context/m68k_set_context
    uint8_t  m68k_ctx[2048];   // buffer de contexto para Musashi

    z80      cpu_snd;          // Z80 de sonido

    /* ── Comunicación inter-CPU ──────────────────────────────────── */
    uint8_t  soundlatch;       // main → sound
    bool     snd_nmi_pending;

    /* ── IRQ state ───────────────────────────────────────────────── */
    int      irq_level;        // nivel IRQ activo (2, 3, 4 o 0)

    /* ── Input ───────────────────────────────────────────────────── */
    // 0x500001: P1 joystick+buttons (active-low)
    // 0x500003: P2 joystick+buttons
    // 0x500005: coins/start/tilt (active-low)
    // 0x500000: DSW1, 0x500002: DSW2 (high bytes de cada word)
    uint8_t  in[3];    // [0]=P1, [1]=P2, [2]=coins/start
    uint8_t  dsw[2];   // DSW1, DSW2 (default all ON = 0xFF)

    /* ── Control ─────────────────────────────────────────────────── */
    bool     quit;
    bool     turbo_mode;
    int      frame_counter;

    /* ── Flags de carga ──────────────────────────────────────────── */
    bool     have_rom;
    bool     have_gfx;
    bool     have_sndrom;

} SnowBros;

// ---------------------------------------------------------------------------
// Variable global para callbacks de Musashi (instancia única por diseño)
// ---------------------------------------------------------------------------
extern SnowBros* g_sb;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void snowbros_init(SnowBros* sb);
void snowbros_destroy(SnowBros* sb);

int  snowbros_load_rom_even(SnowBros* sb, const char* path);  // bytes pares
int  snowbros_load_rom_odd(SnowBros* sb,  const char* path);  // bytes impares
int  snowbros_load_sndrom(SnowBros* sb,   const char* path);
int  snowbros_load_gfx(SnowBros* sb, const char* path, int offset, int size);

void snowbros_decode_gfx(SnowBros* sb);
void snowbros_run_frame(SnowBros* sb);
void snowbros_render(SnowBros* sb);
void snowbros_handle_key(SnowBros* sb, SDL_Scancode sc, bool pressed);

#endif // SNOWBROS_H
