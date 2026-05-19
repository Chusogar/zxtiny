#ifndef ELEVATOR_H
#define ELEVATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

#define EA_MAINROM_SIZE        0x10000
#define EA_SNDROM_SIZE         0x4000

#define EA_RAM_SIZE            0x0800
#define EA_SNDRAM_SIZE         0x0400

#define EA_CHARRAM_SIZE        0x3000

#define EA_VRAM_SIZE           0x0400

#define EA_PALRAM_SIZE         0x0080
#define EA_SPRITERAM_SIZE      0x0100
#define EA_COLSCROLL_SIZE      0x0060

#define EA_GFXROM_SIZE         0x8000

#define EA_TOTAL_COLORS        64

#define EA_LOG_W               256
#define EA_LOG_H               256
#define EA_SCREEN_W            256
#define EA_SCREEN_H            224
#define EA_VIS_Y0              16

#define EA_SCALE               2

#define EA_CPU_CLOCK           4000000
#define EA_SND_CLOCK           3000000
#define EA_FPS                 60
#define EA_CYCLES_PER_FRAME    (EA_CPU_CLOCK / EA_FPS)
#define EA_SND_CYCLES_PER_FRAME (EA_SND_CLOCK / EA_FPS)

#define EA_CHARS_NUM           256
#define EA_SPRITES_NUM         64

#define EA_CHAR_W              8
#define EA_CHAR_H              8
#define EA_SPR_W               16
#define EA_SPR_H               16

typedef struct ElevatorAction {
    z80 cpu_main;
    z80 cpu_snd;

    uint8_t mainrom[EA_MAINROM_SIZE];
    uint8_t sndrom[EA_SNDROM_SIZE];
    uint8_t gfxrom[EA_GFXROM_SIZE];

    bool have_mainrom;
    bool have_sndrom;
    bool have_gfxrom;

    uint8_t ram[EA_RAM_SIZE];
    uint8_t sndram[EA_SNDRAM_SIZE];

    uint8_t charram[EA_CHARRAM_SIZE];

    uint8_t vram_coll[EA_VRAM_SIZE];
    uint8_t vram[3][EA_VRAM_SIZE];

    uint8_t colscroll[EA_COLSCROLL_SIZE];

    uint8_t spriteram[EA_SPRITERAM_SIZE];

    uint8_t palram[EA_PALRAM_SIZE];

    uint8_t pri_reg;

    uint8_t video_mode;

    uint8_t scroll_h[3];
    uint8_t scroll_v[3];

    uint8_t colorbank_12;
    uint8_t colorbank_3s;

    uint8_t exrom_lo;
    uint8_t exrom_hi;

    bool gfx_dirty;
    bool vram_dirty[3];
    bool palette_dirty;
    bool scroll_dirty;

    uint8_t chars[2][EA_CHARS_NUM][EA_CHAR_H][EA_CHAR_W];
    uint8_t sprites[2][EA_SPRITES_NUM][EA_SPR_H][EA_SPR_W];

    uint32_t palette[EA_TOTAL_COLORS];

    uint32_t planebuf[3][EA_LOG_W * EA_LOG_H];
    uint32_t layerbuf[4][EA_SCREEN_W * EA_SCREEN_H];
    uint32_t framebuffer[EA_SCREEN_W * EA_SCREEN_H];

    uint8_t draworder[32][4];

    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    int scale;

    uint8_t soundlatch;

} ElevatorAction;

void ea_init(ElevatorAction* e);
void ea_destroy(ElevatorAction* e);

int ea_load_mainrom(ElevatorAction* e, const char* path, int offset, int size);
int ea_load_sndrom(ElevatorAction* e, const char* path, int offset, int size);
int ea_load_gfxrom(ElevatorAction* e, const char* path, int offset, int size);

int ea_load_from_dir(ElevatorAction* e, const char* dirpath);

void ea_run_frame(ElevatorAction* e);
void ea_render(ElevatorAction* e);

#endif