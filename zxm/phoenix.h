#ifndef PHOENIX_H
#define PHOENIX_H

/*
 * phoenix.h  -  Emulador de Phoenix (Amstar, 1980)
 *
 * Hardware:
 *   CPU   : Intel 8080 @ 3.072 MHz (emulado con core Z80 en modo 8080-compatible)
 *   ROM   : 16 KB en 0x0000-0x3FFF (8 chips x 2KB: ic45..ic52)
 *   RAM   : 0x4000-0x4FFF, 4KB, DOS PAGINAS seleccionadas por videoreg bit0
 *             Pagina A (ram_page[0]): foreground tiles (0x4000-0x43FF)
 *                                     background tiles (0x4800-0x4BFF)
 *             Pagina B (ram_page[1]): idem, pagina alternativa
 *   Video : Tilemap 32x32 tiles de 8x8px, 2bpp, ROT90
 *             - Foreground (FG): tiles de GFX2 (ic39+ic40), con transparencia pen0
 *               offset VRAM = 0x0000-0x03FF dentro de la pagina activa
 *               columna sx=0 se dibuja sin transparencia (borde)
 *             - Background (BG): tiles de GFX1 (ic23+ic24), con scroll vertical
 *               offset VRAM = 0x0800-0x0BFF dentro de la pagina activa
 *   GFX   : Dos charsets de 256 tiles 8x8 2bpp
 *             GFX1 (background): ic23 (plane0 0x000-0x7FF) + ic24 (plane1 0x800-0xFFF)
 *             GFX2 (foreground): ic39 (plane0 0x000-0x7FF) + ic40 (plane1 0x800-0xFFF)
 *             Layout tile: cada byte = una fila de 8px, bit7=px izquierdo
 *             Bitplanes separados: plane0 en bytes 0..7, plane1 en bytes 0+256*8..
 *   Paleta: 2 PROMs de 256x4 bits (ic40_b=low, ic41_a=high)
 *             6 bits de color: R=bit0, G=bit2, B=bit1 (low) + R=bit0, G=bit2, B=bit1 (high)
 *             Valor canal = 0x55*bit_low + 0xAA*bit_high
 *             128 colores en total (2 bancos de 64), banco seleccionado por videoreg bit1
 *   Videoreg (0x5000): bit0=pagina RAM (0=A, 1=B), bit1=banco paleta
 *   Scroll  (0x5800): scroll vertical del background (BG)
 *   Input   (0x7000): active-low: bit0=coin, bit1=start1, bit2=start2,
 *                      bit4=fire, bit5=right, bit6=left, bit7=shield
 *   DSW     (0x7800): bit7=VBLANK, bits0-6=dip switches
 *   IRQ    : ninguna (ignore_interrupt en MAME)
 *   Sound  : TMS36XX + custom (no implementado)
 *   Pantalla fisica: 208x256 (visible 256x208 antes de rotar, 208 columnas x 256 filas)
 *                    visible_area en MAME: { 0, 31*8-1, 0, 26*8-1 } = 256x208
 *                    Rotada 90 grados CCW: pantalla final 208x256 pixels
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Mapa de memoria
// ---------------------------------------------------------------------------
#define PHX_ROM_SIZE        0x4000   // 16 KB
#define PHX_RAM_PAGE_SIZE   0x1000   // 4 KB por pagina
#define PHX_NUM_RAM_PAGES   2

#define PHX_ROM_START       0x0000
#define PHX_ROM_END         0x3FFF
#define PHX_RAM_START       0x4000
#define PHX_RAM_END         0x4FFF
#define PHX_VIDEOREG_START  0x5000
#define PHX_VIDEOREG_END    0x53FF
#define PHX_SCROLL_START    0x5800
#define PHX_SCROLL_END      0x5BFF
#define PHX_SND_A_START     0x6000
#define PHX_SND_A_END       0x63FF
#define PHX_SND_B_START     0x6800
#define PHX_SND_B_END       0x6BFF
#define PHX_INPUT_START     0x7000
#define PHX_INPUT_END       0x73FF
#define PHX_DSW_START       0x7800
#define PHX_DSW_END         0x7BFF

// Offsets dentro de la pagina RAM
#define PHX_FG_VRAM_OFF     0x0000   // foreground tiles: 0x0000-0x03FF (32x13=416)
#define PHX_BG_VRAM_OFF     0x0800   // background tiles: 0x0800-0x0BBF (32x26=832? solo 0x340)
#define PHX_VRAM_SIZE       0x0340   // videoram_size en MAME

// ---------------------------------------------------------------------------
// GFX
// ---------------------------------------------------------------------------
#define PHX_GFX_ROM_SIZE    0x1000   // 4 KB por charset (2 planos x 256 tiles x 8 bytes)
#define PHX_NUM_TILES       256
#define PHX_TILE_W          8
#define PHX_TILE_H          8

// ---------------------------------------------------------------------------
// Color PROMs
// ---------------------------------------------------------------------------
#define PHX_PROM_SIZE       0x0100   // 256 entradas por PROM
#define PHX_NUM_COLORS      128      // 2 bancos x 64 colores

// Colortable: GFX1 usa indices 0-63, GFX2 usa indices 64-127
// Dentro de cada banco: 8 grupos de color x 4 colores por tile (2bpp)
// (code>>5) da el grupo de paleta del tile (0-7)
#define PHX_PALETTE_COLORS  128      // total entradas de paleta ARGB

// ---------------------------------------------------------------------------
// Video - pantalla logica antes de rotar
// ---------------------------------------------------------------------------
// MAME visible_area: { 0*8, 31*8-1, 0*8, 26*8-1 } = 256 x 208 pixels
// Tilemap fisico: 32 columnas x 32 filas de tiles 8x8
// Solo visible: columnas 0-31 (256px), filas 0-25 (208px)
// Tras ROT90 CCW: ancho=208, alto=256
#define PHX_TILES_X         32
#define PHX_TILES_Y         32
#define PHX_VIS_TILES_X     32       // columnas visibles
#define PHX_VIS_TILES_Y     26       // filas visibles (0..25*8 = 208px)
#define PHX_LOG_W           (PHX_VIS_TILES_X * PHX_TILE_W)  // 256
#define PHX_LOG_H           (PHX_VIS_TILES_Y * PHX_TILE_H)  // 208
// Tras ROT90:
#define PHX_SCREEN_W        PHX_LOG_H   // 208
#define PHX_SCREEN_H        PHX_LOG_W   // 256
#define PHX_SCALE           2

// ---------------------------------------------------------------------------
// Temporalizacion
// ---------------------------------------------------------------------------
#define PHX_CPU_CLOCK       3072000  // 3.072 MHz
#define PHX_FPS             60
#define PHX_CYCLES_PER_FRAME  (PHX_CPU_CLOCK / PHX_FPS)  // 51200
// VBLANK: bit7 del DSW, activo-bajo cuando hay VBLANK
// El juego lee el DSW continuamente; alternamos el bit cada frame
#define PHX_VBLANK_FRAMES   1        // 1 frame = VBLANK activo, 1 = inactivo

// ---------------------------------------------------------------------------
// Input (0x7000, active-low)
// ---------------------------------------------------------------------------
#define PHX_IN_COIN     0x01
#define PHX_IN_START1   0x02
#define PHX_IN_START2   0x04
#define PHX_IN_UNUSED   0x08
#define PHX_IN_FIRE     0x10
#define PHX_IN_RIGHT    0x20
#define PHX_IN_LEFT     0x40
#define PHX_IN_SHIELD   0x80

// ---------------------------------------------------------------------------
// Tiles decodificados: [tile_idx][row][col] = valor 0-3 (2bpp)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t pix[PHX_NUM_TILES][PHX_TILE_H][PHX_TILE_W];
} PhxGfx;

// ---------------------------------------------------------------------------
// Estructura principal
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;

    // Memoria
    uint8_t rom[PHX_ROM_SIZE];
    uint8_t ram[PHX_NUM_RAM_PAGES][PHX_RAM_PAGE_SIZE];

    // GFX ROMs crudas
    uint8_t gfx1_rom[PHX_GFX_ROM_SIZE];  // background charset (ic23+ic24)
    uint8_t gfx2_rom[PHX_GFX_ROM_SIZE];  // foreground charset (ic39+ic40)

    // Tiles decodificados
    PhxGfx gfx1;  // background
    PhxGfx gfx2;  // foreground

    // Color PROMs
    uint8_t prom_lo[PHX_PROM_SIZE];  // ic40_b: bits bajos
    uint8_t prom_hi[PHX_PROM_SIZE];  // ic41_a: bits altos

    // Paleta ARGB computada: [banco][color_idx] con banco=0/1
    // Indexacion: color = prom_idx, banco seleccionado por palette_bank
    // Total 128 entradas (2 bancos x 64)
    uint32_t palette[PHX_NUM_COLORS];

    // Estado de video
    int  ram_page;       // pagina activa (0 o 1), bit0 de videoreg
    int  palette_bank;   // banco de paleta activo (0 o 1), bit1 de videoreg
    uint8_t bg_scroll;   // scroll vertical del BG

    // Dirty buffer: marca tiles BG que necesitan redibujarse
    bool dirty_bg[PHX_VRAM_SIZE];

    // Framebuffer: ancho=PHX_SCREEN_W, alto=PHX_SCREEN_H (ya rotado)
    uint32_t framebuffer[PHX_SCREEN_W * PHX_SCREEN_H];

    // Bitmap intermedio (sin rotar): PHX_LOG_W x PHX_LOG_H
    uint32_t logbuf[PHX_LOG_W * PHX_LOG_H];
    // Bitmap intermedio FG (sin rotar): mismo tamano, con alpha=0 para transparencias
    uint32_t fgbuf[PHX_LOG_W * PHX_LOG_H];

    // SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;  // PHX_SCREEN_W x PHX_SCREEN_H

    // Input
    uint8_t input;   // IN0 (0x7000), active-low
    uint8_t dsw;     // DSW (0x7800)

    // VBLANK: bit7 del DSW se pone a 0 durante VBLANK
    bool vblank;

    // Control
    bool quit;
    bool turbo_mode;
    int  frame_counter;

    // Flag de ROMs cargadas
    bool have_rom;
    bool have_gfx1;
    bool have_gfx2;
    bool have_proms;

} Phoenix;

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void phoenix_init(Phoenix* p);
void phoenix_destroy(Phoenix* p);

// Carga ROMs de programa (concatenadas en orden ic45..ic52, total 16KB)
// o individualmente
int phoenix_load_rom(Phoenix* p, const char* path);
int phoenix_load_rom_chunk(Phoenix* p, const char* path, int offset, int size);

// Carga GFX ROM (charset): gfx=1 para BG (ic23+ic24), gfx=2 para FG (ic39+ic40)
int phoenix_load_gfx(Phoenix* p, int gfx, const char* path, int offset, int len);

// Carga PROMs de color: which=0 para low (ic40_b), which=1 para high (ic41_a)
int phoenix_load_prom(Phoenix* p, int which, const char* path);

// Decodifica tiles y calcula paleta (llamar tras cargar todas las ROMs)
void phoenix_decode_gfx(Phoenix* p);
void phoenix_build_palette(Phoenix* p);

void phoenix_run_frame(Phoenix* p);
void phoenix_render(Phoenix* p);
void phoenix_handle_key(Phoenix* p, SDL_Scancode sc, bool pressed);

#endif // PHOENIX_H
