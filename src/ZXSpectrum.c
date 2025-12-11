//================================================================
//                                                              //
// GALAKSIJA emulator                                           //
// DOS Verzija: Copyright (C) by Miodrag Jevremovic 1997.       //
// Win32 Verzija: copyright (C) by Tomaž Šolc 2002 (?).         //
// Linux (SDL2) Verzija: Copyright (C) by Peter Bakota 2017.    //
//                                                              //
//================================================================

#include <stdio.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include <unistd.h>
#include <stdbool.h>

#include "cpu/Z80/Z80.h"

//#define EXECZ80

typedef uint32_t u32;

SDL_Renderer *renderer;
SDL_Window *window;
SDL_Surface *screen;

char *_snapshot;

//static SDL_Renderer* renderer = NULL;
//static SDL_Texture* texture = NULL;

u32 crna_color, pixel_color;

void text_at(const char *str, word x, byte y);

//=========================
//                       //
// GENERALNE PROMENLJIVE //
//                       //
//=========================


// CPU_SPEED is the speed of the CPU in Hz. It is used together with
// FRAMES_PER_SECOND to calculate how much CPU cycles must pass between
// interrupts.
#define CPU_SPEED_NORMAL 3072000 // 6.144/2 Mhz
#define CPU_SPEED_FAST 25000000  // 25 Mhz?
#define FRAMES_PER_SECOND 50

#define SCREEN_W 256
#define SCREEN_H 192
#define SPECTRUM_SCREEN_WIDTH	256
#define SPECTRUM_SCREEN_HEIGHT	192

#define WINDOW_W 256
#define WINDOW_H 192

Z80 R;
byte MEMORY[65536]; // max 64k of MEMORY

uint32_t screen_buffer[SPECTRUM_SCREEN_HEIGHT * SPECTRUM_SCREEN_WIDTH];

byte Fassst;
byte ExitLoop;
u32 last_time = 0;
float sekunda = 0;
float render_time = 0;
byte active_help = 0;

int cpu_speed = CPU_SPEED_NORMAL;

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

bool load_file_to_buffer(const char* path/*, char* buffer*/) {
	FILE *fp;
	long lSize;
	

	fp = fopen ( path , "rb" );
	if( !fp ) return false;

	fseek( fp , 0L , SEEK_END);
	lSize = ftell( fp );
	rewind( fp );

	/* allocate memory for entire content */
	_snapshot = calloc( 1, lSize+1 );
	if( !_snapshot ) fclose(fp),fputs("memory alloc fails",stderr),exit(1);

	/* copy the file into the buffer */
	if( 1!=fread( _snapshot , lSize, 1 , fp) )
	  fclose(fp),free(_snapshot),fputs("entire read fails",stderr),exit(1);

	/* do your work here, buffer is a string contains the whole text */

	fclose(fp);

	return 1;
}

/*
bool load_file_to_buffer(const char* path, vector<uint8_t> &out) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	if (size < 0) { fclose(f); return false; }
	rewind(f);
	out.resize(static_cast<size_t>(size));
	size_t rd = fread(out.data(), 1, out.size(), f);
	fclose(f);
	return rd == out.size();
}
*/

bool loadSNA(const char* filename/*, MinZX* targetEmulator*/)
{
	//char* _buffer;
	if (!load_file_to_buffer(filename /*, _buffer*/)) {
		fprintf(stderr, "Failed to load ROM from %s\n", filename);
		return false;
	}

	//targetEmulator->reset();
	ResetZ80(&R);

	// Read in the registers

	//z80->setRegI(fgetc(pf));
	R.I = _snapshot[0];
	//z80->setRegLx(fgetc(pf));
	//z80->setRegHx(fgetc(pf));
	//R.HL1.W = (_snapshot[2] << 8) + _snapshot[1];
	R.HL1.B.l = _snapshot[1];
	R.HL1.B.h = _snapshot[2];

	//z80->setRegEx(fgetc(pf));
	//z80->setRegDx(fgetc(pf));
	//R.DE1.W = (_snapshot[4] << 8) + _snapshot[3];
	R.DE1.B.l = _snapshot[3];
	R.DE1.B.h = _snapshot[4];

	//z80->setRegCx(fgetc(pf));
	//z80->setRegBx(fgetc(pf));
	//R.BC1.W = (_snapshot[6] << 8) + _snapshot[5];
	R.BC1.B.l = _snapshot[5];
	R.BC1.B.h = _snapshot[6];

	//z80->setRegFx(fgetc(pf));
	//z80->setRegAx(fgetc(pf));
	//R.AF1.W = (_snapshot[8] << 8) + _snapshot[7];
	R.AF1.B.l = _snapshot[7];
	R.AF1.B.h = _snapshot[8];

	//z80->setRegL(fgetc(pf));
	//z80->setRegH(fgetc(pf));
	//R.HL.W = (_snapshot[10] << 8) + _snapshot[9];
	R.HL.B.l = _snapshot[9];
	R.HL.B.h = _snapshot[10];

	//z80->setRegE(fgetc(pf));
	//z80->setRegD(fgetc(pf));
	//R.DE.W = (_snapshot[12] << 8) + _snapshot[11];
	R.DE.B.l = _snapshot[11];
	R.DE.B.h = _snapshot[12];

	//z80->setRegC(fgetc(pf));
	//z80->setRegB(fgetc(pf));
	//R.BC.W = (_snapshot[14] << 8) + _snapshot[13];
	R.BC.B.l = _snapshot[13];
	R.BC.B.h = _snapshot[14];

	//z80->setRegIY(fgetWordLE(pf));
	//R.IY.W = (_snapshot[16] << 8) + _snapshot[15];
	R.IY.B.l = _snapshot[15];
	R.IY.B.h = _snapshot[16];

	//z80->setRegIX(fgetWordLE(pf));
	//R.IX.W = (_snapshot[18] << 8) + _snapshot[17];
	R.IX.B.l = _snapshot[17];
	R.IX.B.h = _snapshot[18];

	//uint8_t inter = fgetc(pf);
	//z80->setIFF2(inter & 0x04 ? 1 : 0);
	R.IFF = (_snapshot[19] & 0x04 ? 1 : 0) ? (R.IFF |= IFF_2) : (R.IFF &= ~IFF_2);

	//z80->setRegR(fgetc(pf));
	R.R = _snapshot[20];

	//z80->setRegAF(fgetWordLE(pf));
	//R.AF.W = (_snapshot[22] << 8) + _snapshot[21];
	R.AF.B.l = _snapshot[21];
	R.AF.B.h = _snapshot[22];

	//z80->setRegSP(fgetWordLE(pf));
	//R.SP.W = (_snapshot[24] << 8) + _snapshot[23];
	R.SP.B.l = _snapshot[23];
	R.SP.B.h = _snapshot[24];

	//z80->setIM((Z80::IntMode)fgetc(pf));
	if (_snapshot[25]==2) { R.IFF |= IFF_IM2; }
	if (_snapshot[25]==1) { R.IFF |= IFF_IM1; }
	
	//targetEmulator->setBorderColor(fgetc(pf));

	//z80->setIFF1(z80->isIFF2());
	R.IFF = (_snapshot[19] & 0x04 ? 1 : 0) ? (R.IFF |= IFF_1) : (R.IFF &= ~IFF_1);
	
	//byte inter = lhandle.read();
	//_zxCpu.iff2 = (inter & 0x04) ? 1 : 0;
	//_zxCpu.r = lhandle.read();

	//_zxCpu.registers.byte[Z80_F] = lhandle.read();
	//_zxCpu.registers.byte[Z80_A] = lhandle.read();

	//sp_l = lhandle.read();
	//sp_h = lhandle.read();
	//_zxCpu.registers.word[Z80_SP] = sp_l + sp_h * 0x100;

	//_zxCpu.im = lhandle.read();
	//byte bordercol = lhandle.read();

	//ESPectrum::borderColor = bordercol;

	//_zxCpu.iff1 = _zxCpu.iff2;
	//std::cout << "loadSNA5 ";
	uint16_t bytesToRead = 0xC000;
	uint16_t offset = 0x4000;
	int _count = 27;
	while (bytesToRead--) {
		MEMORY[offset++] = _snapshot[_count++];
	}
	

	//speccy->z80Regs->PC.B.l = speccy->z80_peek(speccy->z80Regs->SP.W);
	//R.PC.B.l = MEMORY[R.SP.W];
    //speccy->z80Regs->SP.W++;
	//R.SP.W = R.SP.W++;
    //speccy->z80Regs->PC.B.h = speccy->z80_peek(speccy->z80Regs->SP.W);
	//R.PC.B.h = MEMORY[R.SP.W];
    //speccy->z80Regs->SP.W++;
	//R.SP.W = R.SP.W++;
	
	//uint16_t SP = Z80::getRegSP();
    //Z80::setRegPC(MemESP::readword(SP));
	//R.PC.W = MEMORY[R.SP.W];
	R.PC.W = 0x72;
    //Z80::setRegSP(SP + 2);
	//R.SP.W = R.SP.W+2;

	//std::cout << "loadSNA6 ";
	//fclose(pf);
	//std::cout << "loadSNA7 ";

	//uint16_t buf_p = 0x4000;
	//while (lhandle.available()) {
	//	writebyte(buf_p, lhandle.read());
	//	buf_p++;
	//}

	//uint16_t thestack = _zxCpu.registers.word[Z80_SP];
	//retaddr = readword(thestack);
	//Serial.printf("%x\n", retaddr);
	//_zxCpu.registers.word[Z80_SP]++;
	//_zxCpu.registers.word[Z80_SP]++;

	//lhandle.close();

	//_zxCpu.pc = retaddr;

	//targetEmulator->setINT();

	//z80->setRegPC(0x72);

	return 0;

}

//uint8_t _keyboard_state[0xffff];

uint8_t readKeyboard(uint16_t port)
{
	//printf("Port: %d\n", port);
	int ret = 0xff;
	const byte *_keyboard_state = SDL_GetKeyboardState(NULL);
	//_keyboard_state[SDL_SCANCODE_] = 1;

	//printf("%d\n", key[SDL_SCANCODE_A]);
#if 1
	if ((port & 0x0100) == 0) {
		//ret &= (isKeyDown(KeyEvent.VK_SHIFT)) ? ~1 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_LSHIFT]) ? ~1 : 255;
		////ret &= (isKeyDown(KEY_SHIFT)) ? ~1 : 255;
		//ret &= (isKeyDown(KeyEvent.VK_Z)) ? ~2 : 255;
		ret &= (_keyboard_state[SDL_SCANCODE_Z]==1) ? ~2 : 255;
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
		ret &= (_keyboard_state[SDL_SCANCODE_Y]) ? ~16 : 255;
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

// Z80 stuffs
inline byte RdZ80(word addr)
{
    return MEMORY[addr & 0xffff];
}

inline void WrZ80(word addr, byte val)
{
    if (addr >= 0x4000)
    {
        MEMORY[addr] = val;
    }
}

inline byte InZ80(word port)
{
    uint8_t hiport = port >> 8;
	uint8_t loport = port & 0xFF;

	uint8_t result = 0xff;

	
	if ((port & 1) == 0) { // ULA
		result &= readKeyboard(port) /*& _last_read<<2*/ & 0xBF; // abu simbel now works!
		//return result;
		//printf("IN! %d\n", port);
		//return (254 & 0xBF); 

		return result;
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

inline void OutZ80(word port, byte val)
{
    //printf("OUT: %02x at %d\n", val, port);
#if 0
  spectrum* const p = (spectrum*) z->userdata;

  // setting the interrupt vector
  if (port == 0) {
    p->int_vector = val;
  }
#endif
}

inline void PatchZ80(Z80 *R)
{
    // UNUSED
}

#if 0
void show_help()
{
    SDL_FillRect(screen, NULL, crna_color);
    SDL_LockSurface(screen);

    text_at("ZX Spectrum EMULATOR (C)2025 V0.2 ", 0, 0);
    text_at("--------------------------------", 0, 1);
    text_at("F1          - TOGGLE HELP       ", 0, 2);
    text_at("ESC         - QUIT EMULATOR     ", 0, 3);
    text_at("F12 + SHIFT - NORMAL RESET      ", 0, 4);
    text_at("F12         - NMI RESET         ", 0, 5);
    text_at("F8          - TOGGLE CPU SPEED  ", 0, 6);
    text_at("F2 + SHIFT  - SAVE MEMORY  (GTP)", 0, 7);
    text_at("F2          - LOAD MEMORY  (GTP)", 0, 8);

    SDL_UnlockSurface(screen);
}
#endif

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

uint32_t getPaletteColor(int color_index) {
	return spectrum_palette[color_index];
}

#if 0
void put_pixel32( SDL_Texture *surface, int x, int y, Uint32 pixel )
{
    //Convert the pixels to 32 bit
    Uint32 *pixels = (Uint32 *)surface->pixels;
    
    //Set the pixel
    pixels[ ( y * surface->w ) + x ] = 0xffffffff;
}

void update_screen() {
  int pitch = 0;
  void* pixels = NULL;
  if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) {
    SDL_Log("Unable to lock texture: %s", SDL_GetError());
  } else {
    SDL_memcpy(pixels, screen_buffer, pitch * SPECTRUM_SCREEN_HEIGHT);
  }
  SDL_UnlockTexture(texture);
}
#endif

/*
Source - https://stackoverflow.com/a
Posted by unwind, modified by community. See post 'Timeline' for change history
Retrieved 2025-12-10, License - CC BY-SA 4.0
*/

void set_pixel(SDL_Surface *surface, int x, int y, Uint32 pixel)
{
  Uint32 * const target_pixel = (Uint32 *) ((Uint8 *) surface->pixels
                                             + y * surface->pitch
                                             + x * surface->format->BytesPerPixel);
  *target_pixel = pixel;
}


void refresh_screen(void)
{
#if 0
    byte x, y;
    word adresa;

    // 0x2800 - 0x2a00 (size=512) VIDEO MEMORIJA
    adresa = 0x2800;

    SDL_FillRect(screen, NULL, crna_color);
    SDL_LockSurface(screen);

    for (y = 0; y < 16; y++)
    {
        for (x = 0; x < 32; x++)
        {
            draw_char(MEMORY[adresa++], x * SIRINA, y * VISINA);
        }
    }

    SDL_UnlockSurface(screen);
#endif

	SDL_LockSurface(screen);

	//byte *ptr = screen->pixels;

	int x, y;
	//int _posi = 0;
    for (y = 0; y < SPECTRUM_SCREEN_HEIGHT; y++) {
        for (x = 0; x < SPECTRUM_SCREEN_WIDTH; x++) {
			//int _posi = (y * SPECTRUM_SCREEN_WIDTH + x);
            int byte_pos = get_pixel_address(x, y)+0x4000;//((y * WIDTH + x) / 8); // Determinamos la posicion del byte correspondiente
            //int bit_pos = (y * SPECTRUM_SCREEN_WIDTH + x) % 8;  // Determinamos el bit en el byte
            //int _bit_is_set = (scr_data[byte_pos+6144] >> (7 - bit_pos)) & 1; // Extraemos el color (bit 0 o 1)
			int _bit_is_set = ((MEMORY[byte_pos]) >> (7 - (x%8))) & 1;
			int _attr = MEMORY[get_attribute_address(x, y) + 0x4000 + 6144];
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
			uint32_t color = getPaletteColor(color_index);
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
			set_pixel(screen, x, y, color);
			//_posi++;
        }
    }

	SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, screen);

	SDL_Rect src = {0, 0, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT},
             dst = {0, 0, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT};

        SDL_RenderCopy(renderer, tex, &src, &dst);

	SDL_UnlockSurface(screen);
#if 0
	for (int _i=0 ; _i<100 ; _i++ )
	{
		put_pixel32( screen, _i, _i, 0xffffffff );
	}
#endif
	for (int _i=0 ; _i<100 ; _i++ )
	{
		screen_buffer[_i] = 0xffffffff;
	}
	for (int _i=0 ; _i<100 ; _i++ )
	{
		//put_pixel32( texture, _i, _i, 0xffffffff );
	}
	//update_screen();

}

void read_keyboard(void)
{
	 //byte A;

    //const byte *key = SDL_GetKeyboardState(NULL);
	//_keyboard_state[SDL_SCANCODE_] = 1;

	//printf("%d\n", key[SDL_SCANCODE_A]);
/*
    for (A = 1; A < 54; A++)
    {
        // Par modifikacija, radi udobnosti.
        // Shift je Shift, pa bio on levi ili desni. I tome slicno.

        switch (Kmap[A])
        {
        case SDL_SCANCODE_LEFT:
            MEMORY[0x2000 + A] = (key[Kmap[A]] || key[SDL_SCANCODE_BACKSPACE]) ? 0xFE : 0xFF;
            break;

        case SDL_SCANCODE_LSHIFT:
            MEMORY[0x2000 + A] = (key[Kmap[A]] || key[SDL_SCANCODE_RSHIFT]) ? 0xFE : 0xFF;
            break;

        default:
            MEMORY[0x2000 + A] = key[Kmap[A]] ? 0xFE : 0xFF;
        }
    }
*/
}

word LoopZ80(Z80 *R)
{
    u32 current_time = SDL_GetTicks();
    float dt = (float)(current_time - last_time) / 1000.0f;
    last_time = current_time;
    //    int frame_ready=0;

    sekunda += dt;
    if (sekunda >= 1.0)
    {
        char buffer[256];
        sprintf(buffer, "FPS: %d, SPEED=%s", (int)(1.0f/dt), (cpu_speed == CPU_SPEED_NORMAL ? "NORMAL" : "FAST"));

        SDL_SetWindowTitle(window, buffer);

        sekunda -= 1.0;
        //frame_ready = 1;
    }

    SDL_Delay((int)(1000/50));

#if 0

    if (active_help)
    {
        //show_help();
		printf("HELP!\n");
    }
    else //if(frame_ready)
    {
        if (!(R->IFF & IFF_2))
        {
            Fassst++;
        }
        else
        {
            Fassst = 0;
        }

        // Ako je EI osvezavaj i ekran i tastaturu, kao i kod prave masine.
        if (!Fassst)
        {
            read_keyboard();
            refresh_screen();
        }
        else
        {
            switch (Fassst)
            {
            // Because screen is made under IRQ, there is no more further screen updates.
            case 1:
                // Need to clear it ?
                SDL_FillRect(screen, NULL, crna_color);
                break;

            // Stay where You are.
            case 2:
                Fassst--;
                break;
            }
        }
    }
#endif

	read_keyboard();
	 refresh_screen();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            ExitLoop = 1;
            break;
        //case SDL_KEYUP:
		//	_keyboard_state[event.key.keysym.sym] = 0;
		//	break;
		case SDL_KEYDOWN:
        {
			//_keyboard_state[event.key.keysym.sym] = 1;
			//printf("%d\n", event.key.keysym.sym);

            //u32 shift = event.key.keysym.mod & KMOD_SHIFT;
            switch (event.key.keysym.sym)
            {
            case SDLK_ESCAPE:
                ExitLoop = 1;
                break;
            case SDLK_F8: // TOGGLE SPEED
                cpu_speed = (cpu_speed == CPU_SPEED_NORMAL) ? CPU_SPEED_FAST : CPU_SPEED_NORMAL;
                R->IPeriod = cpu_speed / FRAMES_PER_SECOND;
                break;
            case SDLK_F1: // TOGGLE HELP SCREEN
                active_help = !active_help;
                break;
            case SDLK_F2: // SAVE/LOAD MEMORY
                /*if (shift)
                    save_memory();
                else
                    load_memory();*/
                break;
            case SDLK_F12: // HARD/NMI RESET
                //if (shift)
                {
                    ResetZ80(R);
                    return INT_NONE;
                }
                //else
                //    return INT_NMI;
                break;
            }
        }
        break;
        }
    }
#if 1
    /* Select the color for drawing. It is set to black here. */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

    // Clear the window
    SDL_RenderClear(renderer);

    // 0x2bb0 = BROJAC ZA POMERANJE SLIKE
    //int yOff = (int)MEMORY[0x2BB0] * 3;

    // 0x2ba8 = HORIZONTALNA POZICIJA TEKSTA
    //int xOff = (int)MEMORY[0x2BA8] * 8;

    /*if (active_help)
    {
        yOff = 0;
    }*/

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, screen);

    //if (yOff)
    //{
        // yOff = 9..6..3..0
        //int o = 9;
        SDL_Rect src = {0, 0, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT},
                 dst = {0, 0, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT};

        SDL_RenderCopy(renderer, tex, &src, &dst);
    /*}
    else
    {
        SDL_Rect src = {0, 0, SCREEN_W, SCREEN_H},
                 dst = {xOff + (WINDOW_W - SCREEN_W * 2) / 2 - 88, (WINDOW_H - SCREEN_H * 2) / 2, 2 * SCREEN_W, 2 * SCREEN_H};

        SDL_RenderCopy(renderer, tex, &src, &dst);
    }*/

    // Render the changes above ( which up until now had just happened behind the scenes )
    SDL_RenderPresent(renderer);
    SDL_DestroyTexture(tex);
#endif

    if (ExitLoop)
    {
        return INT_QUIT;
    }

    if (active_help)
    {
        return INT_NONE;
    }

	IntZ80(R, INT_IRQ);

    return INT_IRQ; //(frame_ready) ? INT_IRQ : INT_NONE;
}

byte DebugZ80(Z80 *R)
{
    //printf("%04x\n", (unsigned int)R->PC.W);
    return 1;
}

word start_machine()
{
#if defined(DEBUG)
    R.Trap = 0xFFFF;
    R.Trace = 0;
#endif
    last_time = SDL_GetTicks();

    R.IPeriod = cpu_speed / FRAMES_PER_SECOND;

    ResetZ80(&R);

	if (_snapshot != 0)
	{
		printf("trying to load %s", _snapshot);
		loadSNA( _snapshot );
	}
	

    return RunZ80(&R);
}

void init_memory()
{
    FILE *fp;
    //int n, a, b, x, karakter;
    //word adresa;
    //byte buffer[0x4000];

    // Clear memory
    memset((void *)MEMORY, 0, sizeof(MEMORY));

    if (!(fp = fopen("spectrum.rom", "r")))
    {
        fprintf(stderr, "Can't open spectrum.rom\n");
        exit(-1);
    }
    //fread(buffer, 1, 0x4000, fp);
	fread(&MEMORY[0], 1, 0x4000, fp);
    fclose(fp);

    // clean RAM
    /*for (n = 0x4000; n < 0xFFFF; n++)
    {
        MEMORY[n] = 0x00;
    }*/

    Fassst = 0;
    ExitLoop = 0;
}

void usage(char *arg0)
{
    fprintf(stderr, "\nUsage: %s [<GTP file to load> [-j<address>]] [-c<font color>] [-w<work dir>]\n\n", arg0);
}

int main(int argc, char **argv)
{
    int sizeX = WINDOW_W;
    int sizeY = WINDOW_H;
    //int i;
    //char *ptr;
    //int font_color = 0;

	_snapshot = argv[1];

    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) == -1)
    {
        fprintf(stderr, "Failed to initialize SDL : %s\n", SDL_GetError());
        return -1;
    }
    atexit(SDL_Quit);

    window = SDL_CreateWindow("Window Title", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sizeX, sizeY, SDL_WINDOW_SHOWN);
    if (window == NULL)
    {
        fprintf(stderr, "Failed to create window : %s\n", SDL_GetError());
        return -1;
    }

    screen = SDL_CreateRGBSurface(0, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (screen == NULL)
    {
        fprintf(stderr, "Failed to create screen : %s\n", SDL_GetError());
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL)
    {
        fprintf(stderr, "Failed to create renderer : %s\n", SDL_GetError());
        return -1;
    }

#if 0
	// SDL init
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) !=
      0) {
    SDL_Log("Unable to initialise SDL: %s", SDL_GetError());
    return 1;
  }

  SDL_SetHint(SDL_HINT_BMP_SAVE_LEGACY_FORMAT, "1");

  // create SDL window
  SDL_Window* window = SDL_CreateWindow("ZXtiny", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

  if (window == NULL) {
    SDL_Log("Unable to create window: %s", SDL_GetError());
    return 1;
  }

  SDL_SetWindowMinimumSize(window, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT);

  // create renderer
  renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == NULL) {
    SDL_Log("Unable to create renderer: %s", SDL_GetError());
    return 1;
  }

  SDL_RenderSetLogicalSize(renderer, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT);

  // print info on renderer:
  SDL_RendererInfo renderer_info;
  SDL_GetRendererInfo(renderer, &renderer_info);
  SDL_Log("Using renderer %s", renderer_info.name);

  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_STREAMING, SPECTRUM_SCREEN_WIDTH, SPECTRUM_SCREEN_HEIGHT);
  if (texture == NULL) {
    SDL_Log("Unable to create texture: %s", SDL_GetError());
    return 1;
  }
  #endif
    init_memory();

    word pc = start_machine();
    fprintf(stderr, "Leaving emulator PC=$%04x\n", pc);

    //SDL_FreeSurface(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    return 0;
}
