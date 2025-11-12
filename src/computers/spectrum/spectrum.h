#ifndef EMU_SPECTRUM_H
#define EMU_SPECTRUM_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "../../cpu/z80/z80.h"
//#include "wsg.h"

#define SPECTRUM_CLOCK_SPEED 3072000 // 3.072 MHz (= number of cycles per second)
#define SPECTRUM_FPS 60
#define SPECTRUM_CYCLES_PER_FRAME (PAC_CLOCK_SPEED / PAC_FPS)
#define SPECTRUM_SCREEN_WIDTH	224
#define SPECTRUM_SCREEN_HEIGHT	288

typedef struct spectrum spectrum;
struct spectrum {
  z80 cpu;
  uint8_t rom[0x4000]; // 0x0000-0x4000
  uint8_t ram[0xC000]; // 0x4000-0xffff
  //uint8_t sprite_pos[0x10]; // 0x5060-0x506f

  //uint8_t color_rom[32];
  //uint8_t palette_rom[0x100];
  uint8_t palette[0xf];
  //uint8_t tile_rom[0x1000];
  //uint8_t sprite_rom[0x1000];
  //uint8_t sound_rom1[0x100];
  //uint8_t sound_rom2[0x100];

  //uint8_t tiles[256 * 8 * 8]; // to store predecoded tiles
  //uint8_t sprites[64 * 16 * 16]; // to store predecoded sprites

  uint8_t int_vector;
  bool vblank_enabled;
  bool sound_enabled;
  bool flip_screen;

  // in 0 port
  //bool p1_up, p1_left, p1_right, p1_down, rack_advance, coin_s1, coin_s2,
  //    credits_btn;

  // in 1 port
  //bool board_test, p1_start, p2_start;

  // ppu
  uint8_t screen_buffer[SPECTRUM_SCREEN_HEIGHT * SPECTRUM_SCREEN_WIDTH];
  void (*update_screen)(spectrum* const n);

  // audio
  //wsg sound_chip;
  int audio_buffer_len;
  int16_t* audio_buffer;
  int sample_rate;
  bool mute_audio;
  //void (*push_sample)(pac* const n, int16_t);
};

int spectrum_init(spectrum* const p, const char* rom_dir);
void spectrum_quit(spectrum* const p);
void spectrum_update(spectrum* const p, unsigned int ms);

//void pac_cheat_invincibility(pac* const p);

#endif // EMU_SPECTRUM_H
