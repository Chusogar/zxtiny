#ifndef SHAOLINS_H
#define SHAOLINS_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "mc6809/mc6809.h"   // mismo core que mikie

// ---------------------------------------------------------------------------
// Clocks (según driver MAME; MASTER_CLOCK/12 para CPU y SN1; /6 para SN2)
// ---------------------------------------------------------------------------
#define SHAO_MASTER_CLOCK     18432000
#define SHAO_CPU_CLOCK        (SHAO_MASTER_CLOCK/12)   // 1.536 MHz
#define SHAO_SN1_CLOCK        (SHAO_MASTER_CLOCK/12)   // 1.536 MHz
#define SHAO_SN2_CLOCK        (SHAO_MASTER_CLOCK/6)    // 3.072 MHz

// Refresh aproximado (MAME ~60.606). Usamos float para ciclos/frame.
#define SHAO_FPS              60.606061f

// Audio
#define SHAO_AUDIO_RATE       44100
#define SHAO_AUDIO_SAMPLES    1024

// Video: el hardware trabaja 32*8 x 32*8, visible y=16..239 => 256x224
#define SHAO_RAW_W            256
#define SHAO_RAW_H            256
#define SHAO_VIS_Y0           16
#define SHAO_VIS_H            224
#define SHAO_VIS_W            256

// Presentación por defecto: ROT90 => 224x256
#define SHAO_ROT_W            224
#define SHAO_ROT_H            256
#define SHAO_SCALE            2

// GFX
#define SHAO_NUM_CHARS        512
#define SHAO_NUM_SPRITES      256

typedef uint8_t ShaoCharPix[8][8];
typedef uint8_t ShaoSprPix[16][16];

// SN76489 state (idéntico al estilo mikie)
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
    // CPU
    mc6809__t cpu;

    // Memoria plana 64KB (ROM cargada en su offset real)
    uint8_t mem[0x10000];

    // RAM/VRAM (para claridad; mem[] también las refleja vía callbacks)
    uint8_t ram2[0x400];      // 0x2800-0x2BFF
    uint8_t ram1[0x100];      // 0x3000-0x30FF
    uint8_t sprram[0x300];    // 0x3100-0x33FF
    uint8_t colorram[0x400];  // 0x3800-0x3BFF
    uint8_t videoram[0x400];  // 0x3C00-0x3FFF

    // Registros “IO”
    uint8_t nmi_enable;       // write 0x0000 (bit0 flip, bit1 nmi enable)
    uint8_t palettebank;      // write 0x1800 (0..7)
    uint8_t scroll;           // write 0x2000
    uint8_t scrolly[32];      // scroll por columna (cols 4..31 = scroll+1)

    // ROMs de GFX/PROMS
    uint8_t gfx1[0x4000];
    uint8_t gfx2[0x8000];
    uint8_t proms[0x0500];

    // Decodificado
    ShaoCharPix *char_pix;    // [512]
    ShaoSprPix  *spr_pix;     // [256]

    // Paleta base (256) + colortables (2048 chars + 2048 sprites)
    uint32_t pal_argb[256];
    uint8_t  ct_char[2048];
    uint8_t  ct_spr[2048];

    // Buffers de render
    uint32_t raw[SHAO_RAW_W * SHAO_RAW_H];      // 256x256
    uint32_t vis[SHAO_VIS_W * SHAO_VIS_H];      // 256x224 (recorte visible)
    uint32_t out[SHAO_ROT_W * SHAO_ROT_H];      // 224x256 (rotado)

    // SDL
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    // Audio
    SDL_AudioDeviceID audio_dev;
    SDL_AudioSpec     audio_spec;
    SN76489 sn[2];
    uint8_t sn_latch0;
    uint8_t sn_latch1;

    // Inputs (activos a nivel bajo, como MAME)
    uint8_t in_system;
    uint8_t in_p1;
    uint8_t in_p2;
    uint8_t dsw1;
    uint8_t dsw2;
    uint8_t dsw3;

    // Estado
    bool quit;
    bool rotate90;     // toggle con 'R'
	bool turbo_mode;
} Shaolins;

void shaolins_init(Shaolins *s);
void shaolins_destroy(Shaolins *s);
int  shaolins_load_roms(Shaolins *s, const char *rom_dir);

void shaolins_run_frame(Shaolins *s);
void shaolins_render(Shaolins *s);

void shaolins_handle_key(Shaolins *s, SDL_Scancode sc, bool pressed);

#endif