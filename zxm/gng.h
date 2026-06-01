#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "mc6809/mc6809.h"

// ---------------------------------------------------------------------------
// Clocks / vídeo (MAME: MC6809=6 MHz, 256x224 @ ~59.637 Hz)
// ---------------------------------------------------------------------------
#define GNG_CPU_CLOCK     6000000
#define GNG_FPS           59.637405f  // [1](https://www.arcade-museum.com/tech-center/machine/gngt)[2](https://data.spludlow.co.uk/mame/machine/gng)

// Audio (stub por ahora)
#define GNG_AUDIO_RATE    44100
#define GNG_AUDIO_SAMPLES 1024

// Vídeo visible
#define GNG_W 256
#define GNG_H 224
#define GNG_SCALE 2

// Tilemaps
#define GNG_FG_TILES_X 32
#define GNG_FG_TILES_Y 32
#define GNG_BG_TILES_X 32
#define GNG_BG_TILES_Y 32

// Offsets (MAME tilemap scrolldx/scrolldy y sprite offset)
#define GNG_SCROLL_DX 0
#define GNG_SCROLL_DY 0

// GFX sizes (baseline por ROMset típico: chars=0x4000, tiles=0x18000, sprites=0x18000)
#define GNG_CHR_ROM_SIZE  0x4000
#define GNG_TIL_ROM_SIZE  0x18000
#define GNG_SPR_ROM_SIZE  0x18000

// Números “máximos” razonables para decodificar:
#define GNG_MAX_CHARS   1024   // 8x8, 2bpp (256*4 con attr) [4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)
#define GNG_MAX_TILES   1024   // 16x16, 3bpp (attr con high bits) [4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)
#define GNG_MAX_SPRITES 1024   // MAME usa hasta 0x400 códigos [3](https://github.com/mamedev/mame/blob/master/src/mame/capcom/gng.cpp)[4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)

typedef uint8_t GngCharPix[8][8];      // 0..3
typedef uint8_t GngTilePix[16][16];    // 0..7
typedef uint8_t GngSprPix[16][16];     // 0..15

typedef struct {
    // CPU
    mc6809__t cpu;

    // ROM region “maincpu” en un buffer (para bancos y fija)
    uint8_t mainrom[0x18000];  // 0x00000..0x17fff (carga por offsets tipo MAME)
    uint8_t bank;              // 0..4
    const uint8_t *bank_ptr;   // apunta a inicio del banco (8KB)

    // RAM / VRAM (según mapa MAME) [3](https://github.com/mamedev/mame/blob/master/src/mame/capcom/gng.cpp)[4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)
    uint8_t ram[0x1e00];       // 0x0000-0x1dff
    uint8_t sprram[0x200];     // 0x1e00-0x1fff (sin buffer)
    uint8_t sprbuf[0x200];     // buffer DMA
    uint8_t fgvram[0x800];     // 0x2000-0x27ff (0x400 data + 0x400 attr)
    uint8_t bgvram[0x800];     // 0x2800-0x2fff (0x400 data + 0x400 attr)

    // Scroll regs (2 bytes x/y) [3](https://github.com/mamedev/mame/blob/master/src/mame/capcom/gng.cpp)[4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)
    uint8_t scrollx[2];
    uint8_t scrolly[2];

    // Palette RAM (0x3900 base + 0x3800 ext) [3](https://github.com/mamedev/mame/blob/master/src/mame/capcom/gng.cpp)[4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)
    uint8_t pal[0x100];        // 0x3900-0x39ff
    uint8_t pal_ext[0x100];    // 0x3800-0x38ff
    uint32_t argb[256];

    // Sound latch (0x3a00) [3](https://github.com/mamedev/mame/blob/master/src/mame/capcom/gng.cpp)[4](https://github.com/Robbbert/hbmame/blob/master/src/mame/drivers/gng.cpp)
    uint8_t sound_latch;

    // Inputs (activos LOW) [3](https://github.com/mamedev/mame/blob/master/src/mame/capcom/gng.cpp)
    uint8_t in_system;
    uint8_t in_p1;
    uint8_t in_p2;
    uint8_t dsw1;
    uint8_t dsw2;

    // Flags
    bool quit;
    bool turbo_mode;

    // ROMs GFX
    uint8_t chr[GNG_CHR_ROM_SIZE];
    uint8_t til[GNG_TIL_ROM_SIZE];
    uint8_t spr[GNG_SPR_ROM_SIZE];

    // Decodificado
    GngCharPix *chr_pix;   // [GNG_MAX_CHARS]
    GngTilePix *til_pix;   // [GNG_MAX_TILES]
    GngSprPix  *spr_pix;   // [GNG_MAX_SPRITES]

    int num_chars;
    int num_tiles;
    int num_sprites;

    // Framebuffer
    uint32_t fb[GNG_W * GNG_H];

    // SDL
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    // Audio (stub)
    SDL_AudioDeviceID audio_dev;
    SDL_AudioSpec audio_spec;

} Gng;

void gng_init(Gng *g);
void gng_destroy(Gng *g);
int  gng_load_roms(Gng *g, const char *rom_dir);
void gng_run_frame(Gng *g);
void gng_render(Gng *g);
void gng_handle_key(Gng *g, SDL_Scancode sc, bool pressed);

