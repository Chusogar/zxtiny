#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "../jgz80/z80.h"
#include "tms9918.h"          // ← AÑADIDO

// Pantalla
#define MSX_W     256
#define MSX_H     212
#define MSX_SCALE 1

// Timing
#define MSX_CLOCK_HZ      3579545
#define MSX_FPS           60
#define MSX_CYCLES_FRAME  (MSX_CLOCK_HZ / MSX_FPS)

// Audio
#define MSX_AUDIO_RATE 44100
#define MSX_AUDIO_SPF  (MSX_AUDIO_RATE / MSX_FPS)

typedef enum {
    MSX_MODEL_1 = 1,
    MSX_MODEL_2 = 2
} MSXModel;

// SDL globals
extern SDL_Window*       msx_window;
extern SDL_Renderer*     msx_renderer;
extern SDL_Texture*      msx_texture;
extern SDL_AudioDeviceID msx_audio_dev;

extern uint32_t msx_pixels[MSX_H * MSX_W];

// Teclado y Joystick (sin cambios)
extern uint8_t msx_keymap[11];
extern uint8_t msx_joy[2];

int  msx_init(const char* rom_dir, MSXModel model);
void msx_quit(void);
void msx_reset(void);
void msx_update(void);
void msx_render(void);

bool msx_load_rom(const char* path);
bool msx_load_cas(const char* path);