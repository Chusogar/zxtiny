#ifndef TEHKANWC_H
#define TEHKANWC_H

/*
 * tehkanwc.h  -  Emulador de Tehkan World Cup (Tehkan, 1985)
 *
 * Hardware:
 *   CPU main  : Z80 @ 4.608 MHz  (18.432 / 4), IRQ cada VBLANK
 *   CPU sub   : Z80 @ 4.608 MHz, IRQ cada VBLANK
 *   CPU sound : Z80 @ 4.608 MHz, IRQ cada VBLANK + NMI por soundlatch
 *   RAM compart: 0xC800-0xCFFF (2 KB) compartida entre main y sub
 *
 *   ROM main  : 0x0000-0xBFFF (3 chips x 16KB)
 *                 twc-1.bin  0x0000-0x3FFF
 *                 twc-2.bin  0x4000-0x7FFF
 *                 twc-3.bin  0x8000-0xBFFF
 *   ROM sub   : 0x0000-0x7FFF  twc-4.bin (32KB)
 *   ROM sound : 0x0000-0x3FFF  twc-6.bin (16KB)
 *
 *   FG VRAM   : 0xD000-0xD3FF  tile codes  (32x32 tiles 8x8)
 *   FG CRAM   : 0xD400-0xD7FF  tile attrs
 *                 bits 0-3: color (0-15)
 *                 bit  4:   tile code high bit (+256)
 *                 bit  5:   priority (0=sobre sprites, 1=debajo sprites)
 *                 bit  6:   flipX
 *                 bit  7:   flipY
 *   Palette   : 0xD800-0xDDFF  paletteram (768 bytes)
 *                 formato xxxxBBBBGGGGRRRR little-endian (2 bytes/color)
 *                 = 384 colores de 16 bits: byte0=GGGGRRRRR, byte1=xxxxBBBB
 *                 768 colores indexados en total (GFX1: 0-255, GFX2: 256-383, GFX3: 512-767)
 *
 *   BG VRAM   : 0xE000-0xE7FF  tile codes (32x32 tiles 16x8) = 2 bytes por tile
 *                 byte 0: tile code bajo (0-255)
 *                 byte 1: bits 0-3=color, bits 4-5=code high (tile 0-1023),
 *                         bit6=flipX, bit7=flipY
 *               Layout: sx = (offs%64)*8, sy = (offs/64)*8, offs en pasos de 2
 *               Mapa de 512 pixels de ancho, desplazado por scroll X (9 bits) y scroll Y
 *
 *   Sprites   : 0xE800-0xEBFF  (4 bytes por sprite, 256 sprites max)
 *                 byte 0: tile code bajo
 *                 byte 1: bits 0-2=color, bit3=code_high (+256),
 *                         bit5=sx_high (bit8 de sx efectivo - 0x80),
 *                         bit6=flipX, bit7=flipY
 *                 byte 2: sx (+ (bit5_attr<<3) - 0x80 = ajuste visual)
 *                 byte 3: sy
 *
 *   Scroll X  : 0xEC00 (lo), 0xEC01 (hi) → scrollx = lo + hi*256 (9 bits)
 *   Scroll Y  : 0xEC02 → scrolly (1 byte)
 *
 *   Input     : 0xF800-0xF801 trackball P1 x/y  (delta desde ultimo reset)
 *               0xF802: Coin/Start (bit0=coin1, bit1=coin2, bit2=start1, bit3=start2)
 *               0xF803: button P1 (bit5, active-low)
 *               0xF810-0xF811 trackball P2 x/y
 *               0xF813: button P2 (bit5, active-low)
 *               0xF840: DSW1, 0xF850: DSW2, 0xF870: DSW3
 *
 *   GFX1 (chars)  : 16KB  twc-12.bin
 *     charlayout: 8x8, 4bpp packed nibbles
 *     pixels: { 1*4, 0*4, 3*4, 2*4, 5*4, 4*4, 7*4, 6*4 }  (nibble swapped)
 *     rows: { 0*32..7*32 }, stride: 32 bytes
 *     512 tiles, color base 0
 *
 *   GFX2 (sprites): 32KB  twc-8.bin (0x0000) + twc-7.bin (0x8000)
 *     spritelayout: 16x16, 4bpp packed nibbles
 *     pixels: { 1*4,0*4,3*4,2*4,5*4,4*4,7*4,6*4,
 *               8*32+1*4,8*32+0*4,...,8*32+6*4 }
 *     rows: { 0*32..7*32, 16*32..23*32 }, stride: 128 bytes
 *     512 sprites, color base 256
 *     transparente: pen 0
 *
 *   GFX3 (bg tiles): 32KB  twc-11.bin (0x0000) + twc-9.bin (0x8000)
 *     tilelayout: 16x8, 4bpp packed nibbles
 *     pixels: { 1*4,0*4,3*4,2*4,5*4,4*4,7*4,6*4,
 *               32*8+1*4,...,32*8+6*4 }
 *     rows: { 0*32..7*32 }, stride: 64 bytes
 *     1024 tiles, color base 512
 *
 *   Paleta: VIDEO_MODIFIES_PALETTE (paletteram en RAM)
 *     Formato xxxxBBBBGGGGRRRR con swap de bytes:
 *     word = paletteram[offs+1]<<8 | paletteram[offs]
 *     R = (word & 0x000F) * 17
 *     G = ((word & 0x00F0) >> 4) * 17
 *     B = ((word & 0x0F00) >> 8) * 17
 *
 *   Visible area: { 0, 255, 16, 239 } = 256 x 224 pixels (ROT0)
 *   Pantalla: 256 x 224
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Mapa de memoria y tamaños
// ---------------------------------------------------------------------------
#define TWC_MAINROM_SIZE    0xC000   // 48 KB
#define TWC_SUBROM_SIZE     0x8000   // 32 KB
#define TWC_SNDROM_SIZE     0x4000   // 16 KB
#define TWC_MAINRAM_SIZE    0x0800   // 2 KB  (0xC000-0xC7FF)
#define TWC_SHARED_SIZE     0x0800   // 2 KB  (0xC800-0xCFFF)
#define TWC_SUBRAM_SIZE     0x4800   // sub RAM propia (0x8000-0xC7FF)
#define TWC_SNDRAM_SIZE     0x0800   // 2 KB  (0x4000-0x47FF)

#define TWC_FGVRAM_START    0xD000
#define TWC_FGVRAM_SIZE     0x0400   // 1 KB tile codes
#define TWC_FGCRAM_START    0xD400
#define TWC_PALRAM_START    0xD800
#define TWC_PALRAM_SIZE     0x0600   // 768 bytes (384 colores x 2 bytes)
#define TWC_BGVRAM_START    0xE000
#define TWC_BGVRAM_SIZE     0x0800   // 2 KB (32x32 tiles x 2 bytes = 1024 entradas)
#define TWC_SPRITERAM_START 0xE800
#define TWC_SPRITERAM_SIZE  0x0400   // 1 KB (256 sprites x 4 bytes)
#define TWC_SCROLL_X_LO     0xEC00
#define TWC_SCROLL_X_HI     0xEC01
#define TWC_SCROLL_Y        0xEC02

// ---------------------------------------------------------------------------
// GFX
// ---------------------------------------------------------------------------
#define TWC_GFX1_SIZE       0x04000  // 16 KB  chars
#define TWC_GFX2_SIZE       0x10000  // 64 KB  sprites (usamos 32KB)
#define TWC_GFX3_SIZE       0x10000  // 64 KB  bg tiles (usamos 32KB)

#define TWC_CHARS_NUM       512
#define TWC_SPRITES_NUM     512
#define TWC_BGTILES_NUM     1024

#define TWC_CHAR_W          8
#define TWC_CHAR_H          8
#define TWC_SPR_W           16
#define TWC_SPR_H           16
#define TWC_TILE_W          16
#define TWC_TILE_H          8

// ---------------------------------------------------------------------------
// Paleta
// ---------------------------------------------------------------------------
#define TWC_TOTAL_COLORS    768      // GFX1: 0-255, GFX2: 256-383, GFX3: 512-767
// Bases de color por GFX
#define TWC_CHAR_COLOR_BASE   0      // GFX1: 16 grupos x 16 colores = 256
#define TWC_SPR_COLOR_BASE  256      // GFX2:  8 grupos x 16 colores = 128
#define TWC_BG_COLOR_BASE   512      // GFX3: 16 grupos x 16 colores = 256

// Dimensiones del tilemap de fondo (BG)
#define TWC_BG_COLS 32
#define TWC_BG_ROWS 32

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
// Visible: 256x224 (visible_area {0,255,16,239})
// BG bitmap intermedio: 512x256 (32 cols x 32 filas de tiles 16x8 = 512x256)
// pero el scroll solo usa 512 de ancho y 256 de alto
#define TWC_SCREEN_W        256
#define TWC_SCREEN_H        224
#define TWC_LOG_W           256
#define TWC_LOG_H           256
#define TWC_BGBMP_W         512      // bitmap BG de doble anchura
#define TWC_BGBMP_H         256
#define TWC_VIS_Y0          16
#define TWC_VIS_Y1          239
#define TWC_SCALE           2

// ---------------------------------------------------------------------------
// Temporización
// ---------------------------------------------------------------------------
#define TWC_CPU_CLOCK       4608000
#define TWC_FPS             60
#define TWC_CYCLES_PER_FRAME  (TWC_CPU_CLOCK / TWC_FPS)  // 76800
#define TWC_SLICES          10       // slices de CPU por frame (sincronización)

// ---------------------------------------------------------------------------
// Estructura principal
// ---------------------------------------------------------------------------
typedef struct {
    // CPUs
    z80 cpu_main;
    z80 cpu_sub;
    z80 cpu_snd;

    // ROMs
    uint8_t mainrom[TWC_MAINROM_SIZE];
    uint8_t subrom[TWC_SUBROM_SIZE];
    uint8_t sndrom[TWC_SNDROM_SIZE];

    // RAMs
    uint8_t mainram[TWC_MAINRAM_SIZE];   // 0xC000-0xC7FF main
    uint8_t shared[TWC_SHARED_SIZE];     // 0xC800-0xCFFF compartida
    uint8_t subram[TWC_SUBRAM_SIZE];     // 0x8000-0xC7FF sub (privada)
    uint8_t sndram[TWC_SNDRAM_SIZE];

    // VRAM compartida entre main y sub
    uint8_t fgvram[TWC_FGVRAM_SIZE];    // 0xD000-0xD3FF
    uint8_t fgcram[TWC_FGVRAM_SIZE];    // 0xD400-0xD7FF
    uint8_t palram[TWC_PALRAM_SIZE];    // 0xD800-0xDDFF
    uint8_t bgvram[TWC_BGVRAM_SIZE];    // 0xE000-0xE7FF
    uint8_t spriteram[TWC_SPRITERAM_SIZE]; // 0xE800-0xEBFF

    // Dirty flags
    bool bg_dirty[TWC_BGVRAM_SIZE / 2]; // un flag por tile BG (2 bytes/tile)

    // Scroll
    uint8_t scroll_x[2];  // [0]=lo, [1]=hi
    uint8_t scroll_y;

    // GFX ROMs
    uint8_t gfx1[TWC_GFX1_SIZE];
    uint8_t gfx2[TWC_GFX2_SIZE];
    uint8_t gfx3[TWC_GFX3_SIZE];

    // Tiles decodificados (heap)
    uint8_t (*char_pix) [TWC_CHAR_H][TWC_CHAR_W];   // [512][8][8]
    uint8_t (*spr_pix)  [TWC_SPR_H] [TWC_SPR_W];   // [512][16][16]
    uint8_t (*tile_pix) [TWC_TILE_H][TWC_TILE_W];  // [1024][8][16]

    // Paleta ARGB
    uint32_t palette[TWC_TOTAL_COLORS];

    // BG bitmap intermedio (doble ancho para scroll)
    uint32_t bgbmp[TWC_BGBMP_W * TWC_BGBMP_H];

    // Framebuffer final
    uint32_t framebuffer[TWC_SCREEN_W * TWC_SCREEN_H];

    // SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;

    // Soundlatch (main → sound) y answer (sound → main)
    uint8_t soundlatch;
    uint8_t sound_answer;
    bool    snd_nmi_pending;

    // Sub CPU halt
    bool sub_halted;

    // Input
    // Trackball: delta acumulado
    int8_t  track0[2];   // P1 x,y delta
    int8_t  track1[2];   // P2 x,y delta
    // Botones (active-low en bit5)
    uint8_t btn0;        // P1 button (0xF803)
    uint8_t btn1;        // P2 button (0xF813)
    uint8_t coins;       // 0xF802 coins+start (active-low)
    uint8_t dsw[3];      // DSW1,2,3

    // Control
    bool quit;
    bool turbo_mode;
    int  frame_counter;

    // Flags
    bool have_mainrom;
    bool have_subrom;
    bool have_sndrom;
    bool have_gfx1;
    bool have_gfx2;
    bool have_gfx3;

} TehkanWC;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void tehkanwc_init(TehkanWC* t);
void tehkanwc_destroy(TehkanWC* t);

int  tehkanwc_load_mainrom(TehkanWC* t, const char* path, int offset, int size);
int  tehkanwc_load_subrom(TehkanWC* t, const char* path);
int  tehkanwc_load_sndrom(TehkanWC* t, const char* path);
int  tehkanwc_load_gfx1(TehkanWC* t, const char* path);
int  tehkanwc_load_gfx2(TehkanWC* t, const char* path, int offset, int size);
int  tehkanwc_load_gfx3(TehkanWC* t, const char* path, int offset, int size);

void tehkanwc_decode_gfx(TehkanWC* t);

void tehkanwc_run_frame(TehkanWC* t);
void tehkanwc_render(TehkanWC* t);
void tehkanwc_handle_key(TehkanWC* t, SDL_Scancode sc, bool pressed);

#endif // TEHKANWC_H