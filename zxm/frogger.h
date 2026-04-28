#ifndef FROGGER_H
#define FROGGER_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Hardware Frogger (Konami, 1981) - basado en Galaxian
//
//   CPU principal:  Z80 @ 3.072 MHz
//   CPU de sonido:  Z80 @ 1.78975 MHz
//   Video:          256x256 tilemap + sprites, paleta PROM
//   Sonido:         2x AY-3-8910
//   Pantalla:       monitor vertical (rotada 90 grados)
// ---------------------------------------------------------------------------

// Temporalizacion principal
#define FROGGER_MAIN_CLOCK      3072000
#define FROGGER_SOUND_CLOCK     1789750
#define FROGGER_FPS             60
#define FROGGER_MAIN_CYCLES_PER_FRAME  (FROGGER_MAIN_CLOCK / FROGGER_FPS) // 51200
#define FROGGER_SOUND_CYCLES_PER_FRAME (FROGGER_SOUND_CLOCK / FROGGER_FPS) // 29829

// Pantalla: el hardware es 256x256 (vertical)
// Al rotar: 224 columnas visibles x 256 filas
#define FROGGER_SCREEN_W  224
#define FROGGER_SCREEN_H  256
#define FROGGER_SCALE     2

// Tilemap: 32 columnas x 28 filas (rotado) de tiles 8x8
#define FROGGER_TILE_SIZE    8
#define FROGGER_TILE_COLS   28
#define FROGGER_TILE_ROWS   32

// Sprites: hasta 8 sprites de 16x16
#define FROGGER_MAX_SPRITES  8
#define FROGGER_SPRITE_SIZE 16

// Mapa de memoria principal (Main CPU)
//   0000-3FFF  ROM (16K)
//   8000-87FF  RAM de trabajo (2K)
//   8800       Watchdog
//   A800-ABFF  Video RAM (1K, tiles)
//   B000-B0FF  Object RAM (sprites + scroll)
//   B808       IRQ enable
//   B80C       Flip Y
//   B810       Flip X
//   C000-FFFF  PPI 8255 (I/O mapeado, sound cmd, inputs)
#define FROGGER_ROM_SIZE     0x4000
#define FROGGER_RAM_START    0x8000
#define FROGGER_RAM_SIZE     0x0800
#define FROGGER_VRAM_START   0xA800
#define FROGGER_VRAM_SIZE    0x0400
#define FROGGER_OBJRAM_START 0xB000
#define FROGGER_OBJRAM_SIZE  0x0100

// Mapa de memoria sonido (Sound CPU)
//   0000-17FF  ROM (6K)
//   4000-43FF  RAM (1K)
//   I/O: port 0x40 AY read/write, port 0x80 AY control
#define FROGGER_SNDROM_SIZE  0x1800
#define FROGGER_SNDRAM_START 0x4000
#define FROGGER_SNDRAM_SIZE  0x0400

// GFX ROMs
#define FROGGER_GFX_SIZE     0x1000  // 4K (2 ROMs x 2K)

// Color PROM
#define FROGGER_PROM_SIZE    32

// AY-3-8910
#define AY_NUM_REGS  16

typedef struct {
    uint8_t regs[AY_NUM_REGS];
    uint8_t latch;
    // Tone counters
    uint16_t tone_count[3];
    uint8_t  tone_output[3];
    // Noise
    uint16_t noise_count;
    uint32_t noise_shift;
    uint8_t  noise_output;
    // Envelope
    uint32_t env_count;
    uint8_t  env_step;
    bool     env_hold;
    bool     env_alternate;
    bool     env_holding;
} AY8910;

// ---------------------------------------------------------------------------
// Estructura principal del emulador Frogger
// ---------------------------------------------------------------------------
typedef struct {
    // CPUs
    z80 main_cpu;
    z80 sound_cpu;

    // Memoria principal
    uint8_t main_rom[FROGGER_ROM_SIZE];
    uint8_t main_ram[FROGGER_RAM_SIZE];
    uint8_t video_ram[FROGGER_VRAM_SIZE];
    uint8_t obj_ram[FROGGER_OBJRAM_SIZE];

    // Memoria de sonido
    uint8_t sound_rom[FROGGER_SNDROM_SIZE];
    uint8_t sound_ram[FROGGER_SNDRAM_SIZE];

    // GFX ROMs (tiles y sprites comparten)
    uint8_t gfx_rom[FROGGER_GFX_SIZE];
    // Tiles decodificados: 256 tiles x 8x8 x 2bpp -> 1 byte por pixel
    uint8_t tiles[256][FROGGER_TILE_SIZE][FROGGER_TILE_SIZE];
    // Sprites decodificados: 64 sprites x 16x16 x 2bpp
    uint8_t sprites[64][FROGGER_SPRITE_SIZE][FROGGER_SPRITE_SIZE];

    // Color PROM -> paleta ARGB8888
    uint8_t  color_prom[FROGGER_PROM_SIZE];
    uint32_t palette[32 * 4];  // 32 entradas de 4 colores cada una

    // Estado de video
    bool irq_enable;
    bool flip_x;
    bool flip_y;

    // PPI 8255 / I/O
    uint8_t sound_cmd;     // comando al CPU de sonido
    bool    sound_irq;     // IRQ pendiente para el CPU de sonido
    uint8_t input[3];      // IN0, IN1, IN2 (bits invertidos)
    uint8_t dip_switches;  // DIP switches

    // Audio
    AY8910 ay[2];
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[882];
    int   audio_pos;

    // SDL video
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t framebuffer[FROGGER_SCREEN_W * FROGGER_SCREEN_H];

    // Scroll por fila (Frogger: cada fila de tiles tiene su propio scroll)
    uint8_t row_scroll[32];

    bool quit;
    bool turbo_mode;
    int  frame_counter;
} Frogger;

// Prototipos publicos
void frogger_init(Frogger* f);
void frogger_destroy(Frogger* f);
int  frogger_load_roms(Frogger* f, const char* rom_dir);
void frogger_run_frame(Frogger* f);
void frogger_render(Frogger* f);

#endif // FROGGER_H
