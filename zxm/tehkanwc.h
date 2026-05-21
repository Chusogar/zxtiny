#ifndef TEHKANWC_H
#define TEHKANWC_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "z80/jgz80/z80.h"     // tu core Z80 [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/z80.h)
#include "ay8910.h"            // módulo externo AY
#include "msm5205.h"           // módulo externo MSM5205

// ---------------------------------------------------------------------------
// Mapa de memoria y tamaños
// ---------------------------------------------------------------------------
#define TWC_MAINROM_SIZE    0xC000   // 48 KB
#define TWC_SUBROM_SIZE     0x8000   // 32 KB
#define TWC_SNDROM_SIZE     0x4000   // 16 KB

#define TWC_MAINRAM_SIZE    0x0800   // 2 KB  (0xC000-0xC7FF)
#define TWC_SHARED_SIZE     0x0800   // 2 KB  (0xC800-0xCFFF)
#define TWC_SUBRAM_SIZE     0x4800   // sub RAM propia (0x8000-0xC7FF)
#define TWC_SNDRAM_SIZE     0x0800   // 2 KB  (0x4000-0x47FF)

#define TWC_FGVRAM_START    0xD000
#define TWC_FGVRAM_SIZE     0x0400   // 1 KB tile codes
#define TWC_FGCRAM_START    0xD400
#define TWC_PALRAM_START    0xD800
#define TWC_PALRAM_SIZE     0x0600   // 768 bytes
#define TWC_BGVRAM_START    0xE000
#define TWC_BGVRAM_SIZE     0x0800   // 2 KB (2 bytes por tile)
#define TWC_SPRITERAM_START 0xE800
#define TWC_SPRITERAM_SIZE  0x0400   // 1 KB (256 sprites * 4)

#define TWC_SCROLL_X_LO     0xEC00
#define TWC_SCROLL_X_HI     0xEC01
#define TWC_SCROLL_Y        0xEC02

// ---------------------------------------------------------------------------
// GFX
// ---------------------------------------------------------------------------
#define TWC_GFX1_SIZE       0x04000  // 16 KB  chars
#define TWC_GFX2_SIZE       0x10000  // 64 KB  sprites
#define TWC_GFX3_SIZE       0x10000  // 64 KB  bg tiles

#define TWC_CHARS_NUM       512
#define TWC_SPRITES_NUM     512
#define TWC_BGTILES_NUM     1024

#define TWC_CHAR_W          8
#define TWC_CHAR_H          8
#define TWC_SPR_W           16
#define TWC_SPR_H           16
#define TWC_TILE_W          16
#define TWC_TILE_H          8

// Dimensiones tilemap BG (el .c usa TWC_BG_COLS)
#define TWC_BG_COLS         32
#define TWC_BG_ROWS         32

// ---------------------------------------------------------------------------
// Paleta
// ---------------------------------------------------------------------------
#define TWC_TOTAL_COLORS       768
#define TWC_CHAR_COLOR_BASE    0
#define TWC_SPR_COLOR_BASE     256
#define TWC_BG_COLOR_BASE      512

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
#define TWC_SCREEN_W        256
#define TWC_SCREEN_H        224

#define TWC_BGBMP_W         512
#define TWC_BGBMP_H         256

#define TWC_VIS_Y0          16
#define TWC_VIS_Y1          239

#define TWC_SCALE           2

// ---------------------------------------------------------------------------
// Temporización
// ---------------------------------------------------------------------------
#define TWC_CPU_CLOCK          4608000
#define TWC_FPS                60
#define TWC_CYCLES_PER_FRAME   (TWC_CPU_CLOCK / TWC_FPS)
#define TWC_SLICES             10

// ---------------------------------------------------------------------------
// Tipos de pixeles decodificados
// ---------------------------------------------------------------------------
typedef uint8_t TWCCharPix[TWC_CHAR_H][TWC_CHAR_W];
typedef uint8_t TWCSprPix[TWC_SPR_H][TWC_SPR_W];
typedef uint8_t TWCTilePix[TWC_TILE_H][TWC_TILE_W];

// ---------------------------------------------------------------------------
// Estructura principal
// ---------------------------------------------------------------------------
typedef struct {
    // CPUs
    z80 cpu_main;
    z80 cpu_sub;
    z80 cpu_snd;

    // ROMs
    uint8_t mainrom[TWC_MAINROM_SIZE];
    uint8_t subrom[TWC_SUBROM_SIZE];
    uint8_t sndrom[TWC_SNDROM_SIZE];

    // RAMs
    uint8_t mainram[TWC_MAINRAM_SIZE];
    uint8_t subram[TWC_SUBRAM_SIZE];
    uint8_t sndram[TWC_SNDRAM_SIZE];
    uint8_t shared[TWC_SHARED_SIZE];

    // VRAM / IO
    uint8_t fgvram[TWC_FGVRAM_SIZE];
    uint8_t fgcram[TWC_FGVRAM_SIZE];
    uint8_t palram[TWC_PALRAM_SIZE];
    uint8_t bgvram[TWC_BGVRAM_SIZE];
    uint8_t spriteram[TWC_SPRITERAM_SIZE];

    uint8_t scroll_x[2];
    uint8_t scroll_y;

    // GFX ROMs
    uint8_t gfx1[TWC_GFX1_SIZE];
    uint8_t gfx2[TWC_GFX2_SIZE];
    uint8_t gfx3[TWC_GFX3_SIZE];

    // Decodificado (heap)
    TWCCharPix *char_pix;
    TWCSprPix  *spr_pix;
    TWCTilePix *tile_pix;

    // Paleta + buffers
    uint32_t palette[TWC_TOTAL_COLORS];
    uint32_t bgbmp[TWC_BGBMP_W * TWC_BGBMP_H];
    uint32_t framebuffer[TWC_SCREEN_W * TWC_SCREEN_H];

    bool bg_dirty[TWC_BGVRAM_SIZE / 2];

    // SDL vídeo
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    // Entrada (trackballs)
    int8_t track0[2];
    int8_t track1[2];
    uint8_t coins;
    uint8_t btn0;
    uint8_t btn1;
    uint8_t dsw[3];

    // Sound latches
    uint8_t soundlatch;
    uint8_t sound_answer;
    bool snd_nmi_pending;

    // Estado
    bool sub_halted;
    bool sub_was_halted;     // <- tu .c lo usa (fallaba) 
    bool quit;
    bool turbo_mode;
    uint32_t frame_counter;

    // Flags de carga
    bool have_mainrom;
    bool have_subrom;
    bool have_sndrom;
    bool have_gfx1;
    bool have_gfx2;
    bool have_gfx3;

    // -----------------------------------------------------------------------
    // AUDIO (porque tu tehkanwc.c lo usa como miembros de TehkanWC)
    // -----------------------------------------------------------------------
    SDL_AudioDeviceID audio_dev;
    SDL_AudioSpec     audio_spec;

    AY8910 ay1;
    AY8910 ay2;
    MSM5205 msm;

    // ADPCM ROM twc-5.bin (normalmente 16KB)
    uint8_t adpcm[0x4000];
    bool    have_adpcm;

    // contador/estado ADPCM (msm_data_offs + toggle)
    uint16_t msm_offs;
    uint8_t  msm_toggle;

} TehkanWC;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void tehkanwc_init(TehkanWC* t);
void tehkanwc_destroy(TehkanWC* t);

int  tehkanwc_load_mainrom(TehkanWC* t, const char* path, int offset, int size);
int  tehkanwc_load_subrom(TehkanWC* t, const char* path);
int  tehkanwc_load_sndrom(TehkanWC* t, const char* path);

int  tehkanwc_load_gfx1(TehkanWC* t, const char* path);
int  tehkanwc_load_gfx2(TehkanWC* t, const char* path, int offset, int size);
int  tehkanwc_load_gfx3(TehkanWC* t, const char* path, int offset, int size);

// si tu .c tiene loader ADPCM como función pública:
int  tehkanwc_load_adpcm(TehkanWC* t, const char* path);

void tehkanwc_decode_gfx(TehkanWC* t);

void tehkanwc_run_frame(TehkanWC* t);
void tehkanwc_render(TehkanWC* t);

void tehkanwc_handle_key(TehkanWC* t, SDL_Scancode sc, bool pressed);

#endif // TEHKANWC_H