#pragma once
/*
 * cpc.h  –  Amstrad CPC 6128 emulator public interface
 *
 * Hardware:
 *   CPU  : Z80A @ 4 MHz (jgz80 core, mismo que ZX Spectrum)
 *   Video: CRTC 6845 + Gate Array Amstrad
 *          - 3 modos: 160×200×16col / 320×200×4col / 640×200×2col
 *          - Paleta de 27 colores físicos, 16 lápices configurables
 *          - Border configurable
 *   Audio: AY-3-8912 (3 canales tono + ruido + envolvente)
 *   Mem  : 128 KB RAM en bancos de 16 KB + 48 KB ROM (OS+BASIC+AMSDOS)
 *   I/O  : PPI 8255 (teclado, cassette, joystick), Gate Array, CRTC
 *
 * ROMs necesarias (colocar en rom_dir):
 *   cpc6128.rom  – 48 KB (OS 16KB + BASIC 16KB + AMSDOS 16KB)
 *   O bien por separado:
 *   os.rom (16KB) + basic.rom (16KB) + amsdos.rom (16KB)
 *
 * Compilar:
 *   gcc cpc_main.c cpc.c cpc_fdc.c jgz80/z80.c -o cpc -lSDL2 -lm
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "../jgz80/z80.h"

// ── Pantalla ─────────────────────────────────────────────────────────────────
// El CPC muestra hasta 384 columnas y 312 líneas (PAL).
// Área visible estándar: 768×272 px (modo 2 máx.) → escalado a 384×272
#define CPC_W       384
#define CPC_H       272
#define CPC_SCALE     1

// ── Timing ───────────────────────────────────────────────────────────────────
#define CPC_CLOCK_HZ    4000000
#define CPC_FPS              50       // PAL
#define CPC_CYCLES_FRAME    (CPC_CLOCK_HZ / CPC_FPS)   // 80000

// ── Audio ────────────────────────────────────────────────────────────────────
#define CPC_AUDIO_RATE       44100
#define CPC_AUDIO_SPF        (CPC_AUDIO_RATE / CPC_FPS)  // 882

// ── Variables SDL (definidas en cpc_main.c) ───────────────────────────────────
extern SDL_Window*       cpc_window;
extern SDL_Renderer*     cpc_renderer;
extern SDL_Texture*      cpc_texture;
extern SDL_AudioDeviceID cpc_audio_dev;

// Frame buffer ARGB CPC_W × CPC_H
extern uint32_t cpc_pixels[CPC_H * CPC_W];

// ── Controles ────────────────────────────────────────────────────────────────
// El CPC usa una matriz de 10 filas × 8 bits (teclado matricial via PPI)
extern uint8_t cpc_keymap[10];   // 0 = tecla pulsada

// ── Modo de vídeo actual ─────────────────────────────────────────────────────
extern int cpc_video_mode;   // 0, 1 o 2

// ── API pública ───────────────────────────────────────────────────────────────
int  cpc_init(const char* rom_dir);
void cpc_quit(void);
void cpc_reset(void);
void cpc_update(void);    // ejecuta un frame
void cpc_render(void);    // pixels[] → SDL
bool cpc_load_sna(const char* path);              // snapshot .SNA de CPC

// ── Soporte de disco (FDC NEC µPD765A) ───────────────────────────────────────
bool cpc_load_dsk(const char* path);              // cargar DSK en unidad A:
bool cpc_load_dsk_drive(const char* path, int drive); // cargar en unidad A:(0) o B:(1)
void cpc_eject_disk(int drive);                   // expulsar disco de unidad
bool cpc_disk_inserted(int drive);                // consultar si hay disco
