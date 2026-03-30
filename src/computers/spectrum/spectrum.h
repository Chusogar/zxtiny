#ifndef EMU_SPECTRUM_H
#define EMU_SPECTRUM_H

#define Z80_JGZ80
//#define Z80_SZ_Z80

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef Z80_JGZ80
#include "../../cpu/Z80/jgz80/z80.h"
#endif

#ifdef Z80_SZ_Z80
#include "../../cpu/Z80/sz80/z80.h"
#endif

//#include "wsg.h"

#define SPECTRUM_CLOCK_SPEED 3500000 // 3.072 MHz (= number of cycles per second)
#define SPECTRUM_FPS 60
#define SPECTRUM_CYCLES_PER_FRAME (SPECTRUM_CLOCK_SPEED / SPECTRUM_FPS)
#define SPECTRUM_SCREEN_WIDTH	256
#define SPECTRUM_SCREEN_HEIGHT	192

// Audio
#define SPECTRUM_SAMPLE_RATE 44100
#define SPECTRUM_SAMPLES_PER_TSTATE (SPECTRUM_SAMPLE_RATE / SPECTRUM_CLOCK_SPEED)
#define SPECTRUM_LENGHT_AUDIO_FRAME (SPECTRUM_SAMPLE_RATE / SPECTRUM_FPS)



/*struct z80_cpu {

#ifdef Z80_JGZ80
#define Z80 z80;
#endif

#ifdef Z80_SZ_Z80
#define z80 z80;
#endif

};*/

typedef struct spectrum spectrum;
struct spectrum {

  z80 cpu;

  uint8_t rom[0x4000]; // 0x0000-0x4000
  uint8_t ram[0xC000]; // 0x4000-0xffff
  //uint8_t sprite_pos[0x10]; // 0x5060-0x506f

  //uint8_t color_rom[32];
  //uint8_t palette_rom[0x100];
  uint32_t palette[0xf];
  //uint8_t border;
  //uint32_t border_screen[320][200] = {0};
  //long _scanline=0;
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
  uint32_t screen_buffer[SPECTRUM_SCREEN_HEIGHT * SPECTRUM_SCREEN_WIDTH];
  void (*update_screen)(spectrum* const n);
  
  // audio
  //wsg sound_chip;
  int audio_buffer_len;
  int16_t* audio_buffer;
  int sample_rate;
  bool mute_audio;
  //void (*push_sample)(pac* const n, int16_t);
  int audio_frame_pos;
};

int spectrum_init(spectrum* const p, const char* rom_dir);
uint32_t getPaletteColor(int color);
void spectrum_quit(spectrum* const p);
void spectrum_update(spectrum* const p, unsigned int ms);
void init_palette(spectrum* const p);

//void pac_cheat_invincibility(pac* const p);

#endif // EMU_SPECTRUM_H
