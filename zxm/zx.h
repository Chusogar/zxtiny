#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"
#include "tzx.h"

// ---------------------------------------------------------------------------
// Temporización TAP (T-states a 3.5 MHz)
// ---------------------------------------------------------------------------
#define TAP_PILOT_PULSE     2168
#define TAP_PILOT_HEADER    8063
#define TAP_PILOT_DATA      3223
#define TAP_SYNC1_PULSE      667
#define TAP_SYNC2_PULSE      735
#define TAP_BIT0_PULSE       855
#define TAP_BIT1_PULSE      1710
#define TAP_PAUSE_CYCLES  3500000

// ---------------------------------------------------------------------------
// Temporización ULA / pantalla completa (48K)
//   Frame = 312 líneas × 224 T-states = 69 888 T-states
//
//   ULA  0..15   → retrazado vertical superior (invisible)
//   ULA 16..63   → borde superior visible  (48 líneas)
//   ULA 64..255  → área de imagen           (192 líneas)
//   ULA 256..303 → borde inferior visible   (48 líneas)
//   ULA 304..311 → retrazado vertical inferior
// ---------------------------------------------------------------------------
#define ULA_TSTATES_PER_LINE    224
#define ULA_LINES_PER_FRAME     312
#define ULA_FIRST_PAPER_LINE     64
#define ULA_FIRST_VISIBLE_LINE   16
#define ULA_LAST_VISIBLE_LINE   303

#define BORDER_LEFT    48
#define BORDER_RIGHT   48
#define BORDER_TOP     48
#define BORDER_BOTTOM  48
#define SCREEN_W      256
#define SCREEN_H      192
#define FULL_W        (BORDER_LEFT + SCREEN_W + BORDER_RIGHT)   // 352
#define FULL_H        (BORDER_TOP  + SCREEN_H + BORDER_BOTTOM)  // 288

#define BORDER_LINE_BUF  ULA_LINES_PER_FRAME

// ---------------------------------------------------------------------------
// Máquina de estados TAP
// ---------------------------------------------------------------------------
typedef enum {
    TAP_STATE_IDLE = 0,
    TAP_STATE_PILOT,
    TAP_STATE_SYNC1,
    TAP_STATE_SYNC2,
    TAP_STATE_DATA,
    TAP_STATE_PAUSE
} TAPState;

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t pos;
    uint32_t block_len;
    uint32_t byte_pos;
    uint8_t  bit_mask;
    TAPState state;
    int      pilot_count;
    int32_t  pulse_cycles;
    uint8_t  ear;
    bool     active;
} TAPPlayer;

// ---------------------------------------------------------------------------
// Selector de fuente de cinta activa
// ---------------------------------------------------------------------------
typedef enum {
    TAPE_NONE = 0,
    TAPE_TAP,
    TAPE_TZX
} TapeSource;

// ---------------------------------------------------------------------------
// Estructura principal del emulador
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;
    uint8_t memory[65536];

    // Gráficos SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t framebuffer[FULL_W * FULL_H];

    // Borde por línea ULA
    uint8_t border_lines[BORDER_LINE_BUF];

    // Audio SDL
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[882];
    int   audio_pos;

    // Reproductores de cinta
    TAPPlayer  tap;
    TZXPlayer  tzx;
    TapeSource tape_src;   // Fuente activa (TAP o TZX)

    bool quit;
    int  frame_counter;
} ZXSpectrum;

// Variables globales ULA
uint8_t border_color;
uint8_t keyboard_matrix[8];
uint8_t ear_bit;
uint8_t mic_bit;

// Prototipos
void spectrum_init(ZXSpectrum* spec);
int  spectrum_load_rom(ZXSpectrum* spec, const char* filename);
int  spectrum_load_sna(ZXSpectrum* spec, const char* filename);
int  spectrum_load_tap(ZXSpectrum* spec, const char* filename);
int  spectrum_load_tzx(ZXSpectrum* spec, const char* filename);
void spectrum_tape_start(ZXSpectrum* spec);
void spectrum_handle_key(ZXSpectrum* spec, SDL_Scancode key, bool pressed);
void spectrum_run_frame(ZXSpectrum* spec);
void spectrum_render(ZXSpectrum* spec);
void spectrum_destroy(ZXSpectrum* spec);

#endif // SPECTRUM_H
