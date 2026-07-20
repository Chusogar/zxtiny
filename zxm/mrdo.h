#ifndef MRDO_H
#define MRDO_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"
#include "sn76489.h"

#define MRDO_MAINROM_SIZE       0x8000
#define MRDO_MAINRAM_SIZE       0x1000
#define MRDO_BGVRAM_SIZE        0x0800
#define MRDO_FGVRAM_SIZE        0x0800
#define MRDO_SPRITERAM_SIZE     0x0100
#define MRDO_GFX_SIZE           0x2000
#define MRDO_PROM_SIZE          0x0080
#define MRDO_TILE_COUNT         512
#define MRDO_SPRITE_COUNT       128
#define MRDO_PALETTE_PENS       320

#define MRDO_VIS_X0             8
#define MRDO_VIS_Y0             32
#define MRDO_NATIVE_W           240
#define MRDO_NATIVE_H           192
#define MRDO_SCREEN_W           MRDO_NATIVE_H
#define MRDO_SCREEN_H           MRDO_NATIVE_W
#define MRDO_SCALE              2

#define MRDO_CPU_CLOCK          UINT64_C(4000000)
#define MRDO_SOUND_CLOCK        UINT32_C(4000000)
#define MRDO_AUDIO_RATE         48000
#define MRDO_AUDIO_SAMPLES      1024
#define MRDO_FPS_NUM            UINT64_C(60)
#define MRDO_FPS_DEN            UINT64_C(1)
#define MRDO_CYCLES_PER_FRAME \
    ((unsigned)((MRDO_CPU_CLOCK * MRDO_FPS_DEN) / MRDO_FPS_NUM))

typedef struct MrDo {
    z80 cpu_main;

    uint8_t mainrom[MRDO_MAINROM_SIZE];
    uint8_t mainram[MRDO_MAINRAM_SIZE];
    uint8_t bgvideoram[MRDO_BGVRAM_SIZE];
    uint8_t fgvideoram[MRDO_FGVRAM_SIZE];
    uint8_t spriteram[MRDO_SPRITERAM_SIZE];

    uint8_t gfx1[MRDO_GFX_SIZE];
    uint8_t gfx2[MRDO_GFX_SIZE];
    uint8_t gfx3[MRDO_GFX_SIZE];
    uint8_t proms[MRDO_PROM_SIZE];

    uint8_t tile_pixels[2][MRDO_TILE_COUNT][8][8];
    uint8_t sprite_pixels[MRDO_SPRITE_COUNT][16][16];

    uint32_t palette[MRDO_PALETTE_PENS];
    uint32_t native_framebuffer[MRDO_NATIVE_W * MRDO_NATIVE_H];
    uint32_t framebuffer[MRDO_SCREEN_W * MRDO_SCREEN_H];

    uint8_t in0;
    uint8_t in1;
    uint8_t dsw1;
    uint8_t dsw2;

    uint8_t flipscreen;
    uint8_t scrollx;
    uint8_t scrolly;

    SN76489 sn1;
    SN76489 sn2;
    SDL_AudioDeviceID audio_device;
    SDL_AudioSpec audio_spec;

    bool quit;
    bool turbo;
    uint32_t frame_counter;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} MrDo;

int  mrdo_init(MrDo *m);
void mrdo_reset(MrDo *m);
void mrdo_destroy(MrDo *m);
int  mrdo_load_romset(MrDo *m, const char *rom_directory);
void mrdo_decode_graphics(MrDo *m);
void mrdo_run_frame(MrDo *m);
void mrdo_render(MrDo *m);
void mrdo_handle_key(MrDo *m, SDL_Scancode scancode, bool pressed);

#endif
