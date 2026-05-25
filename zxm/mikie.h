#ifndef MIKIE2_H
#define MIKIE2_H

/*
 * mikie.h — Emulador de Mikie (Konami, 1984) estilo MAME 0.37b5
 *
 * Este código replica el driver clásico de MAME para:
 *  - Mapa de memoria main CPU (MC6809E):
 *      0000-00ff RAM
 *      2400-2403 Inputs
 *      2500-2501 DSW
 *      2800-2fff RAM bank2 (sprites en 2800-288f)
 *      3000-37ff RAM bank3
 *      3800-3bff Color RAM
 *      3c00-3fff Video RAM
 *      4000-5fff ROM (check)
 *      6000-ffff ROM
 *  - Registros:
 *      2002 irqtrigger (sound)
 *      2006 flipscreen
 *      2007 irq enable
 *      2100 watchdog
 *      2200 palettebank
 *      2400 sound latch
 *  - Video: tilemap 32x32 + sprites, PROMs y palettebank como vidhrdw/mikie.c
 *
 * Nota:
 *  - No ejecutamos la CPU Z80 de sonido (trap simplificado). Si quieres sonido
 *    exacto, integra un core Z80 y ejecuta 06e_n10.bin con el mapa de MAME.
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "mc6809/mc6809.h"

/* CPU (driver MAME antiguo) */
#define MIKIE_CPU_CLOCK        1250000
#define MIKIE_FPS              60
#define MIKIE_CYCLES_PER_FRAME (MIKIE_CPU_CLOCK / MIKIE_FPS)

/* Audio */
#define MIKIE_AUDIO_RATE       44100

/* SN clocks según driver antiguo (aprox.) */
#define MIKIE_SN0_CLOCK        1789750
#define MIKIE_SN1_CLOCK        3579500

/* Pantalla: render interno 256x256, visible y=16..239 => 256x224 */
#define MIKIE_INT_W 256
#define MIKIE_INT_H 256
#define MIKIE_VIS_Y0 16
#define MIKIE_SCREEN_W 256
#define MIKIE_SCREEN_H 224
#define MIKIE_SCALE 2

/* GFX */
#define MIKIE_NUM_CHARS 512
#define MIKIE_NUM_SPR   256
#define MIKIE_TILE_W 8
#define MIKIE_TILE_H 8
#define MIKIE_SPR_W 16
#define MIKIE_SPR_H 16

/* Memoria */
#define MIKIE_ROM_SPACE 0x10000
#define MIKIE_RAM0_SIZE 0x0100
#define MIKIE_RAM2_SIZE 0x0800
#define MIKIE_RAM3_SIZE 0x0800
#define MIKIE_CRAM_SIZE 0x0400
#define MIKIE_VRAM_SIZE 0x0400
#define MIKIE_SPR_RAM_SIZE 0x90

/* ROMs */
#define MIKIE_GFX1_SIZE 0x4000
#define MIKIE_GFX2_SIZE 0x10000
#define MIKIE_PROM_SIZE 0x0500

/* SN76489 state */
typedef struct {
    uint16_t tone_period[3];
    uint16_t tone_counter[3];
    int8_t   tone_out[3];
    uint8_t  volume[4];
    uint8_t  noise_ctrl;
    uint16_t noise_period;
    uint16_t noise_counter;
    uint16_t noise_lfsr;
    int8_t   noise_out;
    uint8_t  latch_reg;
} SN76489;

typedef struct {
    mc6809__t cpu;

    /* address space ROM 0x0000-0xffff (solo lectura) */
    uint8_t rom[MIKIE_ROM_SPACE];

    /* RAM */
    uint8_t ram0[MIKIE_RAM0_SIZE];
    uint8_t ram2[MIKIE_RAM2_SIZE];
    uint8_t ram3[MIKIE_RAM3_SIZE];
    uint8_t colorram[MIKIE_CRAM_SIZE];
    uint8_t videoram[MIKIE_VRAM_SIZE];
    uint8_t spriteram[MIKIE_SPR_RAM_SIZE];

    /* GFX */
    uint8_t gfx1[MIKIE_GFX1_SIZE];
    uint8_t gfx2[MIKIE_GFX2_SIZE];
    uint8_t proms[MIKIE_PROM_SIZE];

    /* Decodificado */
    uint8_t char_pix[MIKIE_NUM_CHARS][MIKIE_TILE_H][MIKIE_TILE_W];
    uint8_t spr_pix0[MIKIE_NUM_SPR][MIKIE_SPR_H][MIKIE_SPR_W];
    uint8_t spr_pix1[MIKIE_NUM_SPR][MIKIE_SPR_H][MIKIE_SPR_W];

    /* Paleta y LUT */
    uint32_t pal_argb[256];
    uint8_t  chr_lut[256];
    uint8_t  spr_lut[256];

    /* Estado */
    uint8_t palettebank; /* 0..7 */
    bool flipscreen;
    bool irq_enable;

    /* Sonido (trap) */
    SN76489 sn[2];
    uint8_t sound_latch;
    uint8_t last_irqtrig;

    /* Inputs */
    uint8_t in0, in1, in2;
    uint8_t in3;
    uint8_t dsw0, dsw1;

    /* Framebuffers */
    uint32_t tmpbitmap[MIKIE_INT_W * MIKIE_INT_H];
    uint32_t framebuffer[MIKIE_SCREEN_W * MIKIE_SCREEN_H];

    /* SDL */
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    SDL_AudioDeviceID audio_dev;

    bool quit;
    bool turbo;
} Mikie2;

void mikie2_init(Mikie2* m);
void mikie2_destroy(Mikie2* m);
int  mikie2_load_roms(Mikie2* m, const char* rom_dir);
void mikie2_run_frame(Mikie2* m);
void mikie2_render(Mikie2* m);

#endif
