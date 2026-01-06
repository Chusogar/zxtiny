/*
 * ZX Spectrum 48K Emulator con SDL2 + JGZ80
 * Ahora con soporte para cargar snapshots .sna (48K)
 *
 * Compilar: gcc emulator.c z80.c -o emulator -lSDL2 -lm
 * Uso: ./emulator [snapshot.sna]   ← opcional
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include "cpu/jgz80/z80.h"  // from https://github.com/carmiker/jgz80

#define SCREEN_WIDTH  256
#define SCREEN_HEIGHT 192
//#define BORDER_SIZE   32
//#define FULL_WIDTH   (SCREEN_WIDTH + 2 * BORDER_SIZE)
//#define FULL_HEIGHT  (SCREEN_HEIGHT + 2 * BORDER_SIZE)
#define SCALE         1
#define v_border_top 48 //visible 48
#define v_border_bottom 56
#define h_border 32
#define FULL_WIDTH   (SCREEN_WIDTH + 2 * h_border)
#define FULL_HEIGHT  (v_border_top + SCREEN_HEIGHT + v_border_bottom)

#define ROM_SIZE      16384
#define RAM_START     16384
#define MEMORY_SIZE   65536
#define CYCLES_PER_FRAME 69888
int cycles_done = 0;

#define SAMPLE_RATE   44100
#define BEEPER_FREQ   880

#define AUDIO_FREQ 44100
#define CPU_FREQ 3500000

uint8_t memory[MEMORY_SIZE];
z80 cpu;

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
uint32_t pixels[FULL_HEIGHT * FULL_WIDTH];

SDL_AudioSpec audio_spec;
SDL_AudioDeviceID audio_dev;
bool beeper_state = false;
int16_t audio_buffer[SAMPLE_RATE / 50];
uint8_t border_color = 7;

uint32_t zx_colors[16] = {
    0x000000, 0x0000D8, 0xD80000, 0xD800D8,
    0x00D800, 0x00D8D8, 0xD8D800, 0xD8D8D8,
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF,
    0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF
};

uint8_t keyboard[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

struct { SDL_Scancode sdl; int row, bit; } keymap[] = {
    {SDL_SCANCODE_0,4,0}, {SDL_SCANCODE_1,3,0}, {SDL_SCANCODE_2,3,1}, {SDL_SCANCODE_3,3,2},
    {SDL_SCANCODE_4,3,3}, {SDL_SCANCODE_5,3,4}, {SDL_SCANCODE_6,4,4}, {SDL_SCANCODE_7,4,3},
    {SDL_SCANCODE_8,4,2}, {SDL_SCANCODE_9,4,1}, {SDL_SCANCODE_A,1,0}, {SDL_SCANCODE_B,7,4},
    {SDL_SCANCODE_C,0,3}, {SDL_SCANCODE_D,1,2}, {SDL_SCANCODE_E,2,2}, {SDL_SCANCODE_F,1,3},
    {SDL_SCANCODE_G,1,4}, {SDL_SCANCODE_H,6,4}, {SDL_SCANCODE_I,5,2}, {SDL_SCANCODE_J,6,3},
    {SDL_SCANCODE_K,6,2}, {SDL_SCANCODE_L,6,1}, {SDL_SCANCODE_M,7,2}, {SDL_SCANCODE_N,7,3},
    {SDL_SCANCODE_O,5,1}, {SDL_SCANCODE_P,5,0}, {SDL_SCANCODE_Q,2,0}, {SDL_SCANCODE_R,2,3},
    {SDL_SCANCODE_S,1,1}, {SDL_SCANCODE_T,2,4}, {SDL_SCANCODE_U,5,3}, {SDL_SCANCODE_V,0,4},
    {SDL_SCANCODE_W,2,1}, {SDL_SCANCODE_X,0,2}, {SDL_SCANCODE_Y,5,4}, {SDL_SCANCODE_Z,0,1},
    {SDL_SCANCODE_SPACE,7,0}, {SDL_SCANCODE_RETURN,6,0},
    {SDL_SCANCODE_LSHIFT,0,0}, {SDL_SCANCODE_LCTRL,7,1},
 //   {SDL_SCANCODE_LEFT,0,4}, //{SDL_SCANCODE_RIGHT,7,2},
//    {SDL_SCANCODE_UP,7,3}, //{SDL_SCANCODE_DOWN,7,4},
    {0,0,0}
};


// ─────────────────────────────────────────────────────────────
// Carga de snapshot .sna (48K)
// ─────────────────────────────────────────────────────────────
bool load_sna(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "No se pudo abrir .sna: %s\n", filename);
        return false;
    }

    // Header: 27 bytes
    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) {
        fclose(f);
        fprintf(stderr, "Archivo .sna incompleto (header)\n");
        return false;
    }

    // Restaurar registros Z80
    cpu.i     = header[0];
    cpu.h_l_   = (header[2] << 8) | header[1];
    cpu.d_e_   = (header[4] << 8) | header[3];
    cpu.b_c_   = (header[6] << 8) | header[5];
    cpu.a_f_   = (header[8] << 8) | header[7];
    cpu.hl    = (header[10] << 8) | header[9];
    cpu.de    = (header[12] << 8) | header[11];
    cpu.bc    = (header[14] << 8) | header[13];
    cpu.iy    = (header[16] << 8) | header[15];
    cpu.ix    = (header[18] << 8) | header[17];
    cpu.iff2  = header[19] & 0x04;  // bit 2 = IFF2
    cpu.r     = header[20];
    cpu.af    = (header[22] << 8) | header[21];
    cpu.sp    = (header[24] << 8) | header[23];
    cpu.interrupt_mode    = header[25];
    border_color = header[26] & 0x07;

    // Leer RAM 16384..65535 (49152 bytes)
    if (fread(&memory[RAM_START], 1, 49152, f) != 49152) {
        fclose(f);
        fprintf(stderr, "Archivo .sna incompleto (RAM)\n");
        return false;
    }
    fclose(f);

    // Restaurar PC: estaba en la pila (emulación del comportamiento real)
    uint16_t sp = cpu.sp;
    cpu.pc = memory[sp+1] << 8 | memory[sp];
    cpu.sp += 2;  // pop PC

    // IFF1 suele ser igual a IFF2 en la mayoría de casos
    cpu.iff1 = cpu.iff2;

    printf("Snapshot .sna cargado: %s\n", filename);
    printf("PC=0x%04X  SP=0x%04X  Border=%d\n", cpu.pc, cpu.sp, border_color);

    return true;
}

// ─────────────────────────────────────────────────────────────
// Funciones existentes (resumidas para brevedad)
// ─────────────────────────────────────────────────────────────

uint8_t read_byte(void* ud, uint16_t addr) { return memory[addr]; }
void write_byte(void* ud, uint16_t addr, uint8_t val) {
    if (addr >= RAM_START) memory[addr] = val;
}

uint8_t port_in(z80* z, uint16_t port) {
    uint8_t res = 0xFF;
    if ((port & 1) == 0) {
        uint8_t hi = port >> 8;
        for (int r = 0; r < 8; r++)
            if ((hi & (1 << r)) == 0) res &= keyboard[r];
    }
    return res;
}

void port_out(z80* z, uint16_t port, uint8_t val) {
    if ((port & 1) == 0) {
        border_color = val & 0x07;
        beeper_state = (val & 0x10) != 0;
		//printf("Border=%d\n",border_color);
    }
}

bool load_rom(const char* fn) {
    FILE* f = fopen(fn, "rb");
    if (!f) return false;
    fread(memory, 1, ROM_SIZE, f);
    fclose(f);
    return true;
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


// Render the screen
void render_screen() {
    // Clear with border color
    uint32_t border_rgb = zx_colors[border_color];
    for (int y = 0; y < FULL_HEIGHT; y++) {
        for (int x = 0; x < FULL_WIDTH; x++) {
            pixels[y * FULL_WIDTH + x] = border_rgb;
        }
    }
#if 1
              int x, y;
              //int _posi = 0;
    for (y = 0; y < SCREEN_HEIGHT; y++) {
        for (x = 0; x < SCREEN_WIDTH; x++) {
                                          //int _posi = (y * SPECTRUM_SCREEN_WIDTH + x);
            int byte_pos = get_pixel_address(x, y)+0x4000;//((y * WIDTH + x) / 8); // Determinamos la posicion del byte correspondiente
            //int bit_pos = (y * SPECTRUM_SCREEN_WIDTH + x) % 8;  // Determinamos el bit en el byte
            //int _bit_is_set = (scr_data[byte_pos+6144] >> (7 - bit_pos)) & 1; // Extraemos el color (bit 0 o 1)
                                          int _bit_is_set = ((memory[byte_pos]) >> (7 - (x%8))) & 1;
                                          int _attr = memory[get_attribute_address(x, y) + 0x4000 + 6144];
                                          int _ink = (int) (_attr & 0b0111);
                                          int _paper = ((_attr & 0x38) /8);//(int)((_attr >> 3) & 0b0111);

                                          bool flash = (_attr & 0x80) != 0;
            bool invert = flash && ((SDL_GetTicks() / 500) % 2);  // Simple flash
                                          
                                          /*if (_switch_BW)
                                          {
                                                         _ink=0;
                                                         _paper=7;
                                          }*/
                                          

                                          int color_index = (_bit_is_set ^ invert) ? _ink : _paper;

            // Seleccionamos el color de la paleta
                                          //uint32_t color = p->spectrum_palette[color_index];
                                          uint32_t color = zx_colors[color_index];
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
                                          
                                          //screen->pixels[_posi++] = color;
                                          //*(u32 *)ptr = color;
            //ptr += screen->format->BytesPerPixel;
                                          //ptr ++;
                                          //set_pixel(screen, x, y, color);
                                          pixels[((y+h_border)*FULL_WIDTH) + x + h_border] = color;
                                          //_posi++;
        }
    }
#endif
    // Update texture
    SDL_UpdateTexture(texture, NULL, pixels, FULL_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static int beeper = 0;
static int tstate_acc = 0;
//static int16_t buf[1024];
static int pos = 0;

void generate_beeper(int t) {
    int samples = SAMPLE_RATE / 50;
    /*for (int i = 0; i < samples; i++) {
        audio_buffer[i] = beeper_state ? 8000 : -8000;
    }*/
	if (pos<samples)
	{
		audio_buffer[pos++] = beeper_state ? 8000 : -8000;
	} else {
		pos = 0;
	}
	
	/*tstate_acc += t;
    int step = CPU_FREQ / AUDIO_FREQ;
    while(tstate_acc >= step){
        tstate_acc -= step;
        audio_buffer[pos++] = beeper ? 8000 : -8000;
        if(pos==1024){
            SDL_QueueAudio(audio_dev,buf,2048);
            pos=0;
        }
    }*/
}

void audio_callback(void* ud, Uint8* stream, int len) {
    int16_t* s = (int16_t*)stream;
    int n = len / 2;
    for (int i = 0; i < n; i++)
        s[i] = audio_buffer[i % (SAMPLE_RATE / 50)];
}

void handle_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
            exit(0);
		if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12)
            z80_reset(&cpu);

        bool pressed = (e.type == SDL_KEYDOWN);
        for (size_t i = 0; keymap[i].sdl; i++) {
            if (e.key.keysym.scancode == keymap[i].sdl) {
                int r = keymap[i].row, b = keymap[i].bit;
                if (pressed) keyboard[r] &= ~(1 << b);
                else         keyboard[r] |=  (1 << b);
            }
        }
    }
}

#if 1
void displayscanline(int y, int f_flash)
{
/*
	if (y<v_border_top)
	{
		uint32_t border_rgb = zx_colors[border_color];
		for (int _y = 0; _y < FULL_HEIGHT; _y++) {
			for (int x = 0; x < FULL_WIDTH; x++) {
				pixels[(_y * FULL_WIDTH) + x] = 0xffff0000;
			}
		}
	}
*/	

#if 1
  int x, row, col, dir_p, dir_a, pixeles, tinta, papel, atributos;

  row = y /*+ v_border_top*/;    // 4 & 32 = graphical screen offset
  col = (y*(h_border + SCREEN_WIDTH + h_border));              // 32+256+32=320  4+192+4=200  (res=320x200)
  //pixels[((y+h_border)*FULL_WIDTH) + x + h_border] = color;
  //col = (y*FULL_WIDTH);
	//col = y*320;

  for (x = 0; x < h_border; x++) {
    pixels[col++] = zx_colors[border_color];
  }

if ((y>v_border_top)&&(y<(v_border_top+SCREEN_HEIGHT)))
{


  dir_p = (((y-v_border_top) & 0xC0) << 5) + (((y-v_border_top) & 0x07) << 8) + (((y-v_border_top) & 0x38) << 2) +0x4000;
  dir_a = 0x1800 + (32 * ((y-v_border_top) >> 3)) +0x4000;
  //printf("Pixel: %d\n",dir_p);

  //dir_p = get_pixel_address(0, (y-v_border_top))+0x4000;
  //dir_a = get_attribute_address(x, y) + 0x4000 + 6144;
  
  for (x = 0; x < 32; x++)
  {
    pixeles=  memory[dir_p++]; //+0x4000
    atributos=memory[dir_a++];
    
    if (((atributos & 0x80) == 0) || (f_flash == 0))
    {
      tinta = (atributos & 0x07) + ((atributos & 0x40) >> 3);
      papel = (atributos & 0x78) >> 3;
    }
    else
    {
      papel = (atributos & 0x07) + ((atributos & 0x40) >> 3);
      tinta = (atributos & 0x78) >> 3;
    }

	//int tinta = (int) (_attr & 0b0111);
	//int papel = ((_attr & 0x38) /8);//(int)((_attr >> 3) & 0b0111);

    pixels[col++] = zx_colors[ ((pixeles & 0x80) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x40) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x20) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x10) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x08) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x04) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x02) ? tinta : papel) ];
    pixels[col++] = zx_colors[ ((pixeles & 0x01) ? tinta : papel) ];
  }
} else {
	for (x = 0; x < (256); x++)
  {
		pixels[col++] = zx_colors[ border_color ];
  }
}

  for (x = 0; x < h_border; x++) {
    pixels[col++] = zx_colors[ border_color ];
  }

  /*for (x = 0; x < h_border; x++) {
    pixels[col++] = zx_colors[ border_color ];
  }*/

  // Clear with border color
    /*uint32_t border_rgb = zx_colors[border_color];
    for (int y = 0; y < FULL_HEIGHT; y++) {
        for (int x = 0; x < FULL_WIDTH; x++) {
            pixels[y * FULL_WIDTH + x] = border_rgb;
        }
    }*/
  //emu_DrawLinePal16(XBuf, WIDTH, HEIGHT, y);
  #endif
	

}

#endif

void update_texture() {
	// Update texture
    SDL_UpdateTexture(texture, NULL, pixels, FULL_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

int main(int argc, char** argv) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    window = SDL_CreateWindow("ZX Spectrum Emulator + .sna", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              FULL_WIDTH * SCALE, FULL_HEIGHT * SCALE, 0);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, FULL_WIDTH, FULL_HEIGHT);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, FULL_WIDTH, FULL_HEIGHT);

    SDL_zero(audio_spec);
    audio_spec.freq = SAMPLE_RATE;
    audio_spec.format = AUDIO_S16SYS;
    audio_spec.channels = 1;
    audio_spec.samples = 1024;
    audio_spec.callback = audio_callback;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
    SDL_PauseAudioDevice(audio_dev, 0);

    if (!load_rom("../roms/spectrum/spectrum.rom")) {
        fprintf(stderr, "Falta zx48.rom\n");
        return 1;
    }

    z80_init(&cpu);
    cpu.read_byte  = read_byte;
    cpu.write_byte = write_byte;
    cpu.port_in    = port_in;
    cpu.port_out   = port_out;

    // Cargar snapshot si se pasó como argumento
    if (argc > 1 && strstr(argv[1], ".sna")) {
        if (!load_sna(argv[1])) {
            fprintf(stderr, "No se pudo cargar snapshot\n");
            // Continúa con estado inicial
        }
    } else {
        // Estado inicial típico después de reset
        cpu.pc = 0x0000;
        cpu.sp = 0x0000;
        cpu.interrupt_mode = 1;
    }

    uint32_t last_frame = SDL_GetTicks();
	//int _line = 0;
	int _num_frames = 0;
	int _flash_act = 0;
    while (true) {
        handle_input();
        //z80_step_n(&cpu, CYCLES_PER_FRAME);
		//while (cycles_done < CYCLES_PER_FRAME) { 
		int _tot_pant = v_border_top + SCREEN_HEIGHT + v_border_bottom;
  
		z80_pulse_irq(&cpu, 1);
			
		for (int _line = 0; _line < _tot_pant; _line++)
		{
			/*for (int _step=0 ; _step<224 ; _step++ )
			{
				int ts = z80_step(&cpu); // devuelve T-states reales 
				cycles_done += ts;
			}*/

			 
			displayscanline(_line, _flash_act);
			
			z80_step_n(&cpu, 224);
			cycles_done += 224;

			generate_beeper(cycles_done);
			
		}
		//cycles_done = 0;
        //render_screen();
        //generate_beeper();

		if (cycles_done >= CYCLES_PER_FRAME)
		{
			_num_frames++;
			cycles_done = 0;
			if (_num_frames == 32)
			{
				_num_frames = 0;
				_flash_act = !_flash_act;
			}
			//render_screen();
			update_texture();
		}
		

        /*uint32_t now = SDL_GetTicks();
        if (now - last_frame < 20) SDL_Delay(20 - (now - last_frame));
        last_frame = now;*/
		SDL_Delay(20);
    }

    SDL_Quit();
    return 0;
}