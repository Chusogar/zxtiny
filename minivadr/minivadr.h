#ifndef MINIVADR_H
#define MINIVADR_H

/*
 * minivadr.h  -  Emulador de Minivader (Taito, 1990)
 *
 * Hardware:
 *   CPU  : Z80 @ 4 MHz (24 MHz / 6)
 *   ROM  : 8 KB en 0x0000-0x1FFF  (d26-01.bin)
 *   RAM  : 8 KB en 0xA000-0xBFFF  (tambien actua como VRAM)
 *   Video: 256x256 pixels, 1bpp, 32 bytes por linea
 *          Area visible: y=[16,239]
 *          Cada byte = 8 pixels, bit7=pixel izquierdo
 *   Input: puerto 0xE008 (active-low)
 *          bit0=izquierda  bit1=derecha  bit2=disparo  bit3=moneda
 *   IRQ  : VBLANK @ 60 Hz (modo IM1, vector 0xFF)
 *   Sound: ninguno
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Mapa de memoria
// ---------------------------------------------------------------------------
#define MINIVADR_ROM_START  0x0000
#define MINIVADR_ROM_END    0x1FFF
#define MINIVADR_ROM_SIZE   0x2000   // 8 KB

#define MINIVADR_RAM_START  0xA000
#define MINIVADR_RAM_END    0xBFFF
#define MINIVADR_RAM_SIZE   0x2000   // 8 KB  (VRAM incluida)

// Direccion de entrada (memory-mapped I/O)
#define MINIVADR_INPUT_ADDR 0xE008

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
#define MINIVADR_SCREEN_W   256
#define MINIVADR_SCREEN_H   256
#define MINIVADR_VIS_Y0     16       // primera linea visible (inclusive)
#define MINIVADR_VIS_Y1     239      // ultima linea visible (inclusive)
#define MINIVADR_VIS_H      (MINIVADR_VIS_Y1 - MINIVADR_VIS_Y0 + 1)  // 224
#define MINIVADR_BYTES_PER_LINE  32  // 256 px / 8 bits = 32 bytes

// Escala de presentacion SDL
#define MINIVADR_SCALE      2

// ---------------------------------------------------------------------------
// Temporalizacion
// ---------------------------------------------------------------------------
#define MINIVADR_CPU_CLOCK  4000000  // 4 MHz
#define MINIVADR_FPS        60
#define MINIVADR_CYCLES_PER_FRAME  (MINIVADR_CPU_CLOCK / MINIVADR_FPS) // 66666

// ---------------------------------------------------------------------------
// Paleta (1bpp): 0=negro, 1=blanco
// ---------------------------------------------------------------------------
#define MINIVADR_COL_BLACK  0xFF000000u
#define MINIVADR_COL_WHITE  0xFFFFFFFFu

// ---------------------------------------------------------------------------
// Estructura principal
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;

    // Memoria
    uint8_t rom[MINIVADR_ROM_SIZE];
    uint8_t ram[MINIVADR_RAM_SIZE];   // 0xA000-0xBFFF, incluye VRAM

    // Estado de video
    // Framebuffer: solo la zona visible [VIS_Y0..VIS_Y1], ancho = SCREEN_W
    uint32_t framebuffer[MINIVADR_SCREEN_W * MINIVADR_VIS_H];

    // SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;  // SCREEN_W x VIS_H

    // Input: byte leido en 0xE008 (active-low, todos los bits a 1 = sin pulsar)
    uint8_t input;

    // Control
    bool quit;
    bool turbo_mode;
    int  frame_counter;

    // Marcador de IRQ pendiente dentro del frame
    bool irq_fired;

} Minivader;

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void minivadr_init   (Minivader* m);
void minivadr_destroy(Minivader* m);
int  minivadr_load_rom(Minivader* m, const char* path);
void minivadr_run_frame(Minivader* m);
void minivadr_render (Minivader* m);
void minivadr_handle_key(Minivader* m, SDL_Scancode sc, bool pressed);

#endif // MINIVADR_H
