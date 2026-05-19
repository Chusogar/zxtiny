#ifndef PBACTION_H
#define PBACTION_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "z80/jgz80/z80.h"

// Relojes
#define PBA_MAIN_HZ   4000000
#define PBA_AUDIO_HZ  3072000
#define PBA_FPS       60

// Video (PCB 256x224 rotada 90º CW)
#define PBA_LOG_W     256
#define PBA_LOG_H     224
#define PBA_SCREEN_W  224
#define PBA_SCREEN_H  256
#define PBA_SCALE     2

#define PBA_TILES_X   32
#define PBA_TILES_Y   32
#define PBA_TILE_W    8
#define PBA_TILE_H    8
#define PBA_VRAM_SIZE 0x400

#define PBA_SPRRAM_SIZE 0x80

#define PBA_PALETTE_RAM_SIZE 0x200
#define PBA_NUM_COLORS       0x100

typedef struct {
    z80 maincpu;
    z80 audiocpu;

    uint8_t rom[0xC000];
    uint8_t ram[0x1000];

    uint8_t videoram_fg[PBA_VRAM_SIZE];
    uint8_t colorram_fg[PBA_VRAM_SIZE];
    uint8_t videoram_bg[PBA_VRAM_SIZE];
    uint8_t colorram_bg[PBA_VRAM_SIZE];

    uint8_t spriteram[PBA_SPRRAM_SIZE];

    uint8_t palram[PBA_PALETTE_RAM_SIZE];
    uint32_t palette[PBA_NUM_COLORS];

    uint8_t fgchars[0x6000];
    uint8_t bgchars[0x10000];
    uint8_t sprites[0x6000];

    uint8_t nmi_mask;
    uint8_t flipscreen;
    uint8_t bg_scroll;

    uint8_t in_p1;
    uint8_t in_p2;
    uint8_t in_sys;
    uint8_t dsw1;
    uint8_t dsw2;

    uint8_t sound_latch;

    uint32_t logbuf[PBA_LOG_W * PBA_LOG_H];
    uint32_t framebuffer[PBA_SCREEN_W * PBA_SCREEN_H];

    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;

    bool quit;
    bool turbo;
} PBAction;

void pbaction_init(PBAction* s);
void pbaction_shutdown(PBAction* s);

int  pbaction_load_from_dir(PBAction* s, const char* dir);
void pbaction_build_palette(PBAction* s);

void pbaction_run_frame(PBAction* s);
void pbaction_render(PBAction* s);
void pbaction_handle_key(PBAction* s, SDL_Scancode sc, bool pressed);

#endif
