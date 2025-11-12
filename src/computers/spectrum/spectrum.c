#include "spectrum.h"

static uint8_t rb(void* userdata, uint16_t addr) {
  spectrum* const p = (spectrum*) userdata;

  // according to https://www.csh.rit.edu/~jerry/arcade/pacman/daves/
  // the highest bit of the address is unused
  addr &= 0xffff;

  if (addr < 0x4000) {
    return p->rom[addr];
  } else {
    return p->ram[addr - 0x4000];
  }

  return 0xff;
}

static void wb(void* userdata, uint16_t addr, uint8_t val) {
  spectrum* const p = (spectrum*) userdata;

  // according to https://www.csh.rit.edu/~jerry/arcade/pacman/daves/
  // the highest bit of the address is unused
  addr &= 0xffff;

  if (addr < 0x4000) {
    // cannot write to rom
  } else {
    p->ram[addr - 0x4000] = val;
  }
}

static uint8_t port_in(z80* const z, uint8_t port) {
  return 0;
}

static void port_out(z80* const z, uint8_t port, uint8_t val) {
  spectrum* const p = (spectrum*) z->userdata;

  // setting the interrupt vector
  if (port == 0) {
    p->int_vector = val;
  }
}

// appends two NULL-terminated strings, creating a new string in the process.
// The pointer returned is owned by the user.
static inline char* append_path(const char* s1, const char* s2) {
  const int buf_size = strlen(s1) + strlen(s2) + 1;
  char* path = calloc(buf_size, sizeof(char));
  if (path == NULL) {
    return NULL;
  }
  snprintf(path, buf_size, "%s%s", s1, s2);
  return path;
}

// copies "nb_bytes" bytes from a file into memory
static inline int load_file(
    const char* filename, uint8_t* memory, size_t nb_bytes) {
  FILE* f = fopen(filename, "rb");
  if (f == NULL) {
    fprintf(stderr, "error: can't open file '%s'.\n", filename);
    return 1;
  }

  size_t result = fread(memory, sizeof(uint8_t), nb_bytes, f);
  if (result != nb_bytes) {
    fprintf(stderr, "error: while reading file '%s'\n", filename);
    return 1;
  }

  fclose(f);
  return 0;
}

// MARK: graphics

// the color palette is stored in color_rom (82s123.7f). Each byte corresponds
// to one color and is composed of three components red, green and blue
// following that pattern: 0bBBGGGRRR.
// Each color component corresponds to a color intensity.
// @TODO: add comment on how to get from color intensity to RGB color.
static inline void get_color(spectrum* const p, uint8_t color_no, uint8_t* r, uint8_t* g, uint8_t* b) {
  /*const uint8_t data = p->color_rom[color_no];
  *r = ((data >> 0) & 1) * 0x21 + ((data >> 1) & 1) * 0x47 +
       ((data >> 2) & 1) * 0x97;
  *g = ((data >> 3) & 1) * 0x21 + ((data >> 4) & 1) * 0x47 +
       ((data >> 5) & 1) * 0x97;
  *b = ((data >> 6) & 1) * 0x51 + ((data >> 7) & 1) * 0xae;*/
}

// Color palettes are defined in palette_rom (82s126.4a): each palette contains
// four colors (one byte for each color).
static inline void get_palette(spectrum* const p, uint8_t pal_no, uint8_t* pal) {
  /*pal_no &= 0x3f;
  pal[0] = p->palette_rom[pal_no * 4 + 0];
  pal[1] = p->palette_rom[pal_no * 4 + 1];
  pal[2] = p->palette_rom[pal_no * 4 + 2];
  pal[3] = p->palette_rom[pal_no * 4 + 3];*/
}

static inline void spectrum_draw(spectrum* const p) {
  
}

// generates audio for one frame
static inline void sound_update(spectrum* const p) {
  if (!p->sound_enabled || p->mute_audio) {
    return;
  }

  // update the WSG (filling the audio buffer)
  //wsg_play(&p->sound_chip, p->audio_buffer, p->audio_buffer_len);

  // resampling the 96kHz audio stream from the WSG into a 44.1kHz one
  /*float d = (float) WSG_SAMPLE_RATE / (float) p->sample_rate;
  for (int i = 0; i < p->sample_rate / SPECTRUM_FPS; i++) {
    int pos = d * (float) i;
    p->push_sample(p, p->audio_buffer[pos]);
  }*/
}

int spectrum_init(spectrum* const p, const char* rom_dir) {
  z80_init(&p->cpu);
  p->cpu.userdata = p;
  p->cpu.read_byte = rb;
  p->cpu.write_byte = wb;
  p->cpu.port_in = port_in;
  p->cpu.port_out = port_out;

  memset(p->rom, 0, sizeof(p->rom));
  memset(p->ram, 0, sizeof(p->ram));
  memset(p->sprite_pos, 0, sizeof(p->sprite_pos));
  memset(p->screen_buffer, 0, sizeof(p->screen_buffer));

  p->int_vector = 0;
  p->vblank_enabled = 0;
  p->sound_enabled = 0;
  p->flip_screen = 0;

  // in 0 port
  /*p->p1_up = 0;
  p->p1_left = 0;
  p->p1_right = 0;
  p->p1_down = 0;
  p->rack_advance = 0;
  p->coin_s1 = 0;
  p->coin_s2 = 0;
  p->credits_btn = 0;*/

  // in 1 port
  /*p->board_test = 0;
  p->p1_start = 0;
  p->p2_start = 0;*/

  // loading rom files
  int r = 0;
  char* file0 = append_path(rom_dir, "spectrum.rom");
  r += load_file(file0, &p->rom[0x0000], 0x4000);
  //char* file1 = append_path(rom_dir, "pacman.6f");
  //r += load_file(file1, &p->rom[0x1000], 0x1000);
  //char* file2 = append_path(rom_dir, "pacman.6h");
  //r += load_file(file2, &p->rom[0x2000], 0x1000);
  //char* file3 = append_path(rom_dir, "pacman.6j");
  //r += load_file(file3, &p->rom[0x3000], 0x1000);
  //char* file4 = append_path(rom_dir, "82s123.7f");
  //r += load_file(file4, p->color_rom, 32);
  //char* file5 = append_path(rom_dir, "82s126.4a");
  //r += load_file(file5, p->palette_rom, 0x100);
  //char* file6 = append_path(rom_dir, "pacman.5e");
  //r += load_file(file6, p->tile_rom, 0x1000);
  //char* file7 = append_path(rom_dir, "pacman.5f");
  //r += load_file(file7, p->sprite_rom, 0x1000);
  //char* file8 = append_path(rom_dir, "82s126.1m");
  //r += load_file(file8, p->sound_rom1, 0x100);
  // char* file9 = append_path(rom_dir, "82s126.3m");
  // r += load_file(file9, p->sound_rom2, 0x100);

  // @TODO: mspacman
  if (0) {
    
  }

  free(file0);
  //free(file1);
  //free(file2);
  //free(file3);
  //free(file4);
  //free(file5);
  //free(file6);
  //free(file7);
  //free(file8);
  // free(file9);

  preload_images(p);
  p->update_screen = NULL;

  // audio
  //wsg_init(&p->sound_chip, p->sound_rom1);
  p->audio_buffer_len = WSG_SAMPLE_RATE / PAC_FPS;
  p->audio_buffer = calloc(p->audio_buffer_len, sizeof(int16_t));
  p->sample_rate = 44100;
  p->mute_audio = false;
  //p->push_sample = NULL;

  return r != 0;
}

void spectrum_quit(spectrum* const p) {
  free(p->audio_buffer);
}

// updates emulation for "ms" milliseconds.
void spectrum_update(spectrum* const p, unsigned int ms) {
  // machine executes exactly PAC_CLOCK_SPEED cycles every second,
  // so we need to execute "ms * PAC_CLOCK_SPEED / 1000"
  int count = 0;
  while (count < ms * SPECTRUM_CLOCK_SPEED / 1000) {
    int cyc = p->cpu.cyc;
    z80_step(&p->cpu);
    int elapsed = p->cpu.cyc - cyc;
    count += elapsed;

    if (p->cpu.cyc >= SPECTRUM_CYCLES_PER_FRAME) {
      p->cpu.cyc -= SPECTRUM_CYCLES_PER_FRAME;

      // trigger vblank if enabled:
      if (p->vblank_enabled) {
        // p->vblank_enabled = 0;
        z80_gen_int(&p->cpu, p->int_vector);

        spectrum_draw(p);
        p->update_screen(p);
        sound_update(p);
      }
    }
  }
}

