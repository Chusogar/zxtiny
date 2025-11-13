#include "spectrum.h"

uint32_t spectrum_palette[16] = {
    0xff000000, /* negro */
	0xff0000bf, /* azul */
	0xffbf0000, /* rojo */
	0xffbf00bf, /* magenta */
	0xff00bf00, /* verde */
	0xff00bfbf, /* ciano */
	0xffbfbf00, /* amarillo */
	0xffbfbfbf, /* blanco */
	0xff000000, /* negro brillante */
	0xff0000ff, /* azul brillante */
	0xffff0000, /* rojo brillante	*/
	0xffff00ff, /* magenta brillante */
	0xff00ff00, /* verde brillante */
	0xff00ffff, /* ciano brillante */
	0xffffff00, /* amarillo brillante */
	0xffffffff  /* blanco brillante */
};


static uint8_t rb(void* userdata, uint16_t addr) {
  spectrum* const p = (spectrum*) userdata;

  // according to https://www.csh.rit.edu/~jerry/arcade/pacman/daves/
  // the highest bit of the address is unused
  addr &= 0xffff;

  if (addr < 0x4000) {
	  //printf("READ ROM: %02x at %04x\n", p->rom[addr], addr);
    return p->rom[addr];
  } else {
    return p->ram[addr - 0x4000];
  }

  return 0x00;
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

static uint8_t readKeyboard(uint16_t port)
{
	int ret = 0xff;
#if 0
	if ((port & 0x0100) == 0) {
		//ret &= (isKeyDown(KeyEvent.VK_SHIFT)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_LSHIFT]) ? ~1 : 255;
		////ret &= (isKeyDown(KEY_SHIFT)) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_Z)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_Z]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_X)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_X]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_C)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_C]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_V)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_V]) ? ~16 : 255;
	}
	if ((port & 0x0200) == 0) {
		//ret &= (isKeyDown(KeyEvent.VK_A)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_A]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_S)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_S]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_D)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_D]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_F)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_F]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_G)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_G]) ? ~16 : 255;
	}
	if ((port & 0x0400) == 0) {
		//ret &= (isKeyDown(KeyEvent.VK_Q)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_Q]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_W)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_W]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_E)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_E]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_R)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_R]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_T)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_T]) ? ~16 : 255;
	}
	if ((port & 0x0800) == 0) {                     
		//ret &= (isKeyDown(KeyEvent.VK_1)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_1]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_2)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_2]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_3)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_3]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_4)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_4]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_5)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_5]) ? ~16 : 255;
	}
	if ((port & 0x1000) == 0) {                        
		//ret &= (isKeyDown(KeyEvent.VK_0)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_0]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_9) || isKeyDown(KeyEvent.VK_UP)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_9]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_8) || isKeyDown(KeyEvent.VK_DOWN)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_8]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_7) || isKeyDown(KeyEvent.VK_RIGHT)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_7]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_6) || isKeyDown(KeyEvent.VK_LEFT)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_6]) ? ~16 : 255;
	}
	if ((port & 0x2000) == 0) {                      
		//ret &= (isKeyDown(KeyEvent.VK_P)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_P]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_O)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_O]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_I)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_I]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_U)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_U]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_Y)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_U]) ? ~16 : 255;
	}
	if ((port & 0x4000) == 0) {                       
		//ret &= (isKeyDown(KeyEvent.VK_ENTER)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_RETURN]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_L)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_L]) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_K)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_K]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_J)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_J]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_H)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_H]) ? ~16 : 255;
	}
	if ((port & 0x8000) == 0) {                     
		//ret &= (isKeyDown(KeyEvent.VK_SPACE)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_SPACE]) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_CONTROL)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_LCTRL]) ? ~2 : 255;
		////ret &= (isKeyDown(KEY_CTRL)) ? ~2 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_M)) ? ~4 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_M]) ? ~4 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_N)) ? ~8 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_N]) ? ~8 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_B)) ? ~16 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_B]) ? ~16 : 255;
		//std::cout << "PULSO! ";
	}
#endif	
	return ret;
}

static uint8_t port_in(z80* const z, uint8_t port) {
	uint8_t hiport = port >> 8;
	uint8_t loport = port & 0xFF;

	uint8_t result = 0xff;

	
	if ((port & 1) == 0) { // ULA
		//result &= readKeyboard(port) /*& _last_read<<2*/ & 0xBF; // abu simbel now works!
		//return result;
	}

	if (loport == 0xFE) {
		result = 0xBF;

		// EAR_PIN
		if (hiport == 0xFE) {
//#ifdef EAR_PRESENT
//			bitWrite(result, 6, digitalRead(EAR_PIN));
//#endif
		}

		// Keyboard
		//if (~(portHigh | 0xFE) & 0xFF) result &= (Ports::base[0] & Ports::wii[0]);
		//if (~(portHigh | 0xFD) & 0xFF) result &= (Ports::base[1] & Ports::wii[1]);
		//if (~(portHigh | 0xFB) & 0xFF) result &= (Ports::base[2] & Ports::wii[2]);
		//if (~(portHigh | 0xF7) & 0xFF) result &= (Ports::base[3] & Ports::wii[3]);
		//if (~(portHigh | 0xEF) & 0xFF) result &= (Ports::base[4] & Ports::wii[4]);
		//if (~(portHigh | 0xDF) & 0xFF) result &= (Ports::base[5] & Ports::wii[5]);
		//if (~(portHigh | 0xBF) & 0xFF) result &= (Ports::base[6] & Ports::wii[6]);
		//if (~(portHigh | 0x7F) & 0xFF) result &= (Ports::base[7] & Ports::wii[7]);

		result &= readKeyboard(port) & 0xBF;

		

		return result;
	}
  return 0xff;
}

static void port_out(z80* const z, uint8_t port, uint8_t val) {
	printf("OUT: %02x at %d\n", val, port);
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

int get_pixel_address(int x, int y) {
	int y76 = y & 0b11000000; //# third of screen
	int y53 = y & 0b00111000;
	int y20 = y & 0b00000111;
	int address = (y76 << 5) + (y20 << 8) + (y53 << 2) + (x >> 3);
	return address;
}

int get_attribute_address(int x, int y) {
	int y73 = y & 0b11111000;
	int address = (y73 << 2) + (x >> 3);
	return address;
}

/*unsigned char  get_byte(unsigned char* scr, int x, int y) {
	return scr[ get_pixel_address(x,y) ];
}

unsigned char  get_attribute(unsigned char* scr, int x, int y) {
	return scr[ get_attribute_address(x,y) + 6144 ];
}*/

static inline void spectrum_draw(spectrum* const p) {
	int x, y;
	int _posi = 0;
    for (y = 0; y < SPECTRUM_SCREEN_HEIGHT; y++) {
        for (x = 0; x < SPECTRUM_SCREEN_WIDTH; x++) {
			//int _posi = (y * SPECTRUM_SCREEN_WIDTH + x);
            int byte_pos = get_pixel_address(x, y);//((y * WIDTH + x) / 8); // Determinamos la posicion del byte correspondiente
            int bit_pos = (y * SPECTRUM_SCREEN_WIDTH + x) % 8;  // Determinamos el bit en el byte
            //int _bit_is_set = (scr_data[byte_pos+6144] >> (7 - bit_pos)) & 1; // Extraemos el color (bit 0 o 1)
			int _bit_is_set = ((p->ram[byte_pos]) >> (7 - (x%8))) & 1;
			int _attr = p->ram[get_attribute_address(x, y)+6144];
			int _ink = (int) (_attr & 0b0111);
			int _paper = ((_attr & 0x38) /8);//(int)((_attr >> 3) & 0b0111);
			
			/*if (_switch_BW)
			{
				_ink=0;
				_paper=7;
			}*/
			

			int color_index = _bit_is_set ? _ink : _paper;

            // Seleccionamos el color de la paleta
			//uint32_t color = p->spectrum_palette[color_index];
			uint32_t color = spectrum_palette[color_index];
			/*if (color_index != 0)
			{
				printf("COLOR: %d\n", color_index);
				printf("POSI: %d\n", byte_pos);
			}*/

			/*if (_posi == 0)
			{
				printf("byte_pos: %d\n", byte_pos);
				printf("attr_addr: %d\n", get_attribute_address(x, y));
			}*/
			
            //Uint32 color = palette[color_index];
            //SDL_SetRenderDrawColor(renderer, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, 255);
            //SDL_RenderDrawPoint(renderer, x, y);
			p->screen_buffer[_posi++] = color;
			//_posi++;
        }
    }  
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
  //memset(p->sprite_pos, 0, sizeof(p->sprite_pos));
  memset(p->screen_buffer, 0, sizeof(p->screen_buffer));

  p->int_vector = 0;
  p->vblank_enabled = 1;
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

  //preload_images(p);
  p->update_screen = NULL;

  // audio
  //wsg_init(&p->sound_chip, p->sound_rom1);
  //p->audio_buffer_len = WSG_SAMPLE_RATE / PAC_FPS;
  p->audio_buffer_len = 0xffff;
  p->audio_buffer = calloc(p->audio_buffer_len, sizeof(int16_t));
  p->sample_rate = 44100;
  p->mute_audio = false;
  //p->push_sample = NULL;

  return r != 0;
}


void init_palette(spectrum* const p) {
	for (int _i=0 ; _i<16 ; _i++ )
	{
		p->palette[_i] = spectrum_palette[_i];
	}	
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

