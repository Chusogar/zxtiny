#ifndef MRDO_H
#define MRDO_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "z80/jgz80/z80.h"
#include "ay8910.h"

// ---------------------------------------------------------------------------
// Mapa de memoria y tamaños (Mr. Do' - Universal, 1982)
// ---------------------------------------------------------------------------
#define MRDO_MAINROM_SIZE       0x4000   // 16 KB (0x0000-0x3FFF)
#define MRDO_MAINRAM_SIZE       0x1800   // 6 KB  (0x4000-0x57FF)

// VRAM (Video RAM) y paleta
#define MRDO_CHARRAM_START      0x6000
#define MRDO_CHARRAM_SIZE       0x0400   // 1 KB (32x32 caracteres)
#define MRDO_COLORRAM_START     0x6400
#define MRDO_COLORRAM_SIZE      0x0400   // 1 KB (color de cada carácter)
#define MRDO_PALRAM_START       0x6800
#define MRDO_PALRAM_SIZE        0x0200   // 512 bytes

// Scroll y controles
#define MRDO_SCROLL_X           0x7000
#define MRDO_SCROLL_Y           0x7001
#define MRDO_SPRITE_BASE        0x7100   // sprites compartidos con video

// ---------------------------------------------------------------------------
// GFX (Graphics ROMs)
// ---------------------------------------------------------------------------
#define MRDO_GFX_SIZE           0x2000   // 8 KB - chars 8x8, 2 bits per pixel
#define MRDO_CHARS_NUM          256      // 256 caracteres únicos
#define MRDO_SPRITES_NUM        4        // 4 sprites simultáneos (básico)

#define MRDO_CHAR_W             8
#define MRDO_CHAR_H             8

// ---------------------------------------------------------------------------
// Paleta
// ---------------------------------------------------------------------------
#define MRDO_TOTAL_COLORS       256
#define MRDO_COLOR_BASE         0

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
#define MRDO_SCREEN_W           256
#define MRDO_SCREEN_H           224
#define MRDO_SCALE              2
#define MRDO_VIS_Y0             0
#define MRDO_VIS_Y1             223

// ---------------------------------------------------------------------------
// Temporización
// ---------------------------------------------------------------------------
#define MRDO_CPU_CLOCK          3000000  // 3 MHz
#define MRDO_FPS                60
#define MRDO_CYCLES_PER_FRAME   (MRDO_CPU_CLOCK / MRDO_FPS)
#define MRDO_SLICES             10

// ---------------------------------------------------------------------------
// Tipos de pixeles decodificados
// ---------------------------------------------------------------------------
typedef uint8_t MrDoCharPix[MRDO_CHAR_H][MRDO_CHAR_W];

// ---------------------------------------------------------------------------
// Estructura principal del emulador
// ---------------------------------------------------------------------------
typedef struct {
    // CPU
    z80 cpu_main;

    // ROMs
    uint8_t mainrom[MRDO_MAINROM_SIZE];
    uint8_t gfx[MRDO_GFX_SIZE];

    // RAMs
    uint8_t mainram[MRDO_MAINRAM_SIZE];
    uint8_t workram[0x0800];  // Area de trabajo adicional

    // VRAM (Memoria de video)
    uint8_t charram[MRDO_CHARRAM_SIZE];      // Códigos de caracteres (1 byte por tile)
    uint8_t colorram[MRDO_COLORRAM_SIZE];    // Atributos de color
    uint8_t palram[MRDO_PALRAM_SIZE];        // Paleta (256 colores x 2 bytes)

    // Sprites inline
    uint8_t spriteram[0x40];  // Sprite RAM embebida (4 sprites x 16 bytes)

    // Scroll
    uint8_t scroll_x;
    uint8_t scroll_y;

    // Decodificado (heap)
    MrDoCharPix *char_pix;

    // Paleta + buffers
    uint32_t palette[MRDO_TOTAL_COLORS];
    uint32_t framebuffer[MRDO_SCREEN_W * MRDO_SCREEN_H];

    bool char_dirty[MRDO_CHARRAM_SIZE];  // Dirty flags para tiles

    // SDL vídeo
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    // Entrada (joystick)
    uint8_t joystick;  // bits 0-3: up, down, left, right; 4-5: button1, button2
    uint8_t dsw[2];    // DIP switches

    // Sonido
    AY8910 ay;
    uint8_t soundlatch;
    uint8_t sound_answer;

    // Estado
    bool quit;
    bool turbo_mode;
    uint32_t frame_counter;

    // Flags de carga
    bool have_mainrom;
    bool have_gfx;

    // Interrupts
    bool irq_pending;
    uint16_t irq_vector;

} MrDo;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void mrdo_init(MrDo* m);
void mrdo_destroy(MrDo* m);

int  mrdo_load_mainrom(MrDo* m, const char* path);
int  mrdo_load_gfx(MrDo* m, const char* path);

void mrdo_decode_gfx(MrDo* m);

void mrdo_run_frame(MrDo* m);
void mrdo_render(MrDo* m);

void mrdo_handle_key(MrDo* m, SDL_Scancode sc, bool pressed);

#endif // MRDO_H
