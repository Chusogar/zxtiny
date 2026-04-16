#pragma once
/*
 * coleco.h  –  ColecoVision emulator public interface
 *
 * Hardware:
 *   CPU  : Z80A @ 3.579545 MHz  (mismo core jgz80)
 *   Video: TMS9918A (implementación completa)
 *          - 256×192 píxeles visibles (NTSC: 262 líneas, 342 ciclos/línea)
 *          - 16 colores exactos (valores RGB medidos en hardware real)
 *          - 16 KB VRAM
 *          - Modos: Gráfico I, Texto 40col, Gráfico II, Multicolor
 *          - 32 sprites por frame, máx. 4 por scanline
 *          - Sprites 8×8 o 16×16 con magnificación ×2
 *          - Early clock (EC bit), detección de colisión por pixel
 *          - Status register: VBlank(F), Coincidence(C), 5th-sprite
 *          - IRQ sincronizado al final del scanline 192
 *          - Pre-fetch del read buffer (comportamiento hardware exacto)
 *          - Máscara de CT/PG en Graphic II (R#3/R#4 bits de expansión)
 *   Audio: SN76489 (3 tonos cuadrados + 1 ruido LFSR, atenuación en dB)
 *   Mem  : 8 KB BIOS ROM (0x0000-0x1FFF)
 *          1 KB RAM espejada (0x6000-0x7FFF)
 *          Hasta 32 KB cartucho (0x8000-0xFFFF)
 *   I/O  : 2 mandos (joystick digital + keypad numérico 12 teclas)
 *          Puerto 0x80 = modo keypad, 0xC0 = modo joystick
 *
 * ROMs:
 *   coleco.rom  – 8 KB BIOS
 *
 * Compilar:
 *   gcc coleco_main.c coleco.c ../jgz80/z80.c -o coleco -lSDL2 -lm
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "../jgz80/z80.h"

#define COLECO_W            256    // píxeles visibles por scanline
#define COLECO_H            192    // scanlines activos (área de imagen)
#define COLECO_SCALE          3    // escala de ventana → 768×576

// Timing NTSC exacto
#define COLECO_CLOCK_HZ   3579545  // frecuencia del cristal Z80/TMS
#define COLECO_FPS               60
#define COLECO_CYCLES_FRAME  (COLECO_CLOCK_HZ / COLECO_FPS)  // ~59659

// Timing interno del TMS9918A
#define TMS_CYCLES_PER_LINE    342  // ciclos Z80 por scanline
#define TMS_LINES_VISIBLE      192  // scanlines activos
#define TMS_LINES_TOTAL        262  // total scanlines por frame (NTSC)

#define COLECO_AUDIO_RATE     44100
#define COLECO_AUDIO_SPF      (COLECO_AUDIO_RATE / COLECO_FPS)  // 735

extern SDL_Window*       cv_window;
extern SDL_Renderer*     cv_renderer;
extern SDL_Texture*      cv_texture;
extern SDL_AudioDeviceID cv_audio_dev;

extern uint32_t cv_pixels[COLECO_H * COLECO_W];

// Mandos: 2 joysticks, cada uno con fire1, fire2 y teclado numérico
extern uint8_t cv_joy[2];    // bits: [0]=Up [1]=Down [2]=Left [3]=Right [4]=Fire1 [5]=Fire2
extern uint8_t cv_keypad[2]; // 0-9,*,# por joystick

int  cv_init(const char* rom_dir);
void cv_quit(void);
void cv_reset(void);
void cv_update(void);
void cv_render(void);
bool cv_load_rom(const char* path);   // cartucho .col o .rom
