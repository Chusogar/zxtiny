#ifndef KUNGFUM_H
#define KUNGFUM_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"
#include "ay8910.h"
#include "msm5205.h"

/* -------------------------------------------------------------------------
 * ROM / RAM
 * ------------------------------------------------------------------------- */
#define KFM_MAINROM_SIZE      0x08000
#define KFM_SOUNDROM_SIZE     0x06000
#define KFM_GFX1_SIZE         0x06000
#define KFM_GFX2_SIZE         0x18000
#define KFM_PROM_SIZE         0x00620

/* BG window ampliada para depuración A000-BFFF */
#define KFM_TILERAM_SIZE      0x02000
#define KFM_TEXTRAM_SIZE      0x00800
#define KFM_SPRITERAM_SIZE    0x0100
#define KFM_WORKRAM_SIZE      0x1000

/* -------------------------------------------------------------------------
 * Video
 * ------------------------------------------------------------------------- */
#define KFM_BG_W              8
#define KFM_BG_H              8
#define KFM_SPR_W             16
#define KFM_SPR_H             16

#define KFM_BG_COLS           64
#define KFM_BG_ROWS           32

#define KFM_SCREEN_W          256
#define KFM_SCREEN_H          256
#define KFM_SCALE             2

#define KFM_BG_COUNT          1024
#define KFM_SPR_COUNT         1024

/* -------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */
#define KFM_CPU_CLOCK         3072000
#define KFM_FPS               56
#define KFM_CYCLES_PER_FRAME  (KFM_CPU_CLOCK / KFM_FPS)
#define KFM_SLICES            8

typedef uint8_t KFMBgPix[KFM_BG_H][KFM_BG_W];
typedef uint8_t KFMSprPix[KFM_SPR_H][KFM_SPR_W];

typedef struct Kungfum {
    /* CPU principal */
    z80 cpu_main;

    /* ROM / RAM */
    uint8_t mainrom[KFM_MAINROM_SIZE];
    uint8_t soundrom[KFM_SOUNDROM_SIZE];
    uint8_t gfx1[KFM_GFX1_SIZE];
    uint8_t gfx2[KFM_GFX2_SIZE];
    uint8_t prom[KFM_PROM_SIZE];

    uint8_t tileram[KFM_TILERAM_SIZE];
    uint8_t textram[KFM_TEXTRAM_SIZE];
    uint8_t spriteram[KFM_SPRITERAM_SIZE];
    uint8_t workram[KFM_WORKRAM_SIZE];

    /* Scroll */
    uint8_t scroll_y_low;
    uint8_t scroll_y_high;
    uint8_t scroll_x_low;
    uint8_t scroll_x_high;

    /* Entradas */
    uint8_t in0;
    uint8_t in1;
    uint8_t in2;
    uint8_t dsw1;
    uint8_t dsw2;

    /* Sonido / control */
    uint8_t sound_latch;
    bool flip_screen;

    /* Estado de carga */
    bool have_mainrom;
    bool have_soundrom;
    bool have_gfx1;
    bool have_gfx2;
    bool have_prom;

    /* GFX decodificadas */
    KFMBgPix  *bg_pix;
    KFMSprPix *spr_pix;

    /* Paleta */
    uint32_t palette[0x200];

    /* Framebuffer */
    uint32_t framebuffer[KFM_SCREEN_W * KFM_SCREEN_H];

    /* SDL */
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_AudioDeviceID audio_dev;

    /* Control */
    bool quit;
} Kungfum;

/* API */
void kungfum_init(Kungfum *k);
void kungfum_destroy(Kungfum *k);

int  kungfum_load_mainrom (Kungfum *k, const char *path, int offset, int size);
int  kungfum_load_soundrom(Kungfum *k, const char *path, int offset, int size);
int  kungfum_load_gfx1    (Kungfum *k, const char *path, int offset, int size);
int  kungfum_load_gfx2    (Kungfum *k, const char *path, int offset, int size);
int  kungfum_load_prom    (Kungfum *k, const char *path, int offset, int size);

void kungfum_decode_gfx(Kungfum *k);
void kungfum_render(Kungfum *k);
void kungfum_run_frame(Kungfum *k);
void kungfum_handle_key(Kungfum *k, SDL_Scancode sc, bool pressed);

#endif /* KUNGFUM_H */