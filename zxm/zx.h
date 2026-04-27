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
//   Línea ULA:  56 T-states de caracteres por línea (=224 T / 4 T/char)
//   Área de imagen: 192 líneas × 128 T-states activos
//   Cada 4 T-states la ULA lee 1 byte de bitmap + 1 byte de atributo
//   y genera 16 píxeles de salida (modo 2× estilo CPCEC)
// ---------------------------------------------------------------------------
#define ULA_TSTATES_PER_LINE    224
#define ULA_LINES_PER_FRAME     312
#define ULA_CHARS_PER_LINE       56  // 224 / 4
#define ULA_FIRST_PAPER_LINE     64
#define ULA_FIRST_VISIBLE_LINE   16
#define ULA_LAST_VISIBLE_LINE   303
#define ULA_TICKS_PER_FRAME   69888  // 312 × 224

#define SCALE			1
#define BORDER_LEFT    48
#define BORDER_RIGHT   48
#define BORDER_TOP     48
#define BORDER_BOTTOM  48
#define SCREEN_W      256
#define SCREEN_H      192
#define FULL_W        (BORDER_LEFT + SCREEN_W + BORDER_RIGHT)   // 352
#define FULL_H        (BORDER_TOP  + SCREEN_H + BORDER_BOTTOM)  // 288

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
// Estructura principal del emulador (estilo CPCEC)
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;
    uint8_t memory[65536];

    // SDL vídeo
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t framebuffer[FULL_W * FULL_H];

    // ── ULA: renderizado por T-state (CPCEC video_main) ──────────────
    int ula_count_x;     // carácter horizontal actual (0..55)
    int ula_count_y;     // línea ULA actual (0..311)
    int ula_count_z;     // sub-T dentro del carácter (0..3)
    int ula_shown_x;     // columna bitmap/attrib (0..31 activa, 32..55 borde/hblank)
    int ula_shown_y;     // línea bitmap activa (0..191), negativo = borde sup
    int ula_bitmap;      // offset VRAM bitmap para la fila actual
    int ula_attrib;      // offset VRAM atributo para la fila actual
    int video_y;         // línea de display (avanza en HBLANK, char 38)

    // ── Contención de memoria (CPCEC ula_clash) ──────────────────────
    uint8_t ula_clash[ULA_TICKS_PER_FRAME]; // retardo de contención por T-state
    int     ula_clash_z; // posición en-frame para consultar ula_clash[]
    int     contention_extra; // contención acumulada en la instrucción actual

    // ── Floating bus ─────────────────────────────────────────────────
    int ula_bus;          // -1 = borde, 0..255 = último atributo leído por ULA

    // ── ULA snow ─────────────────────────────────────────────────────
    int ula_snow_a;       // máscara de snow (31 si habilitado, 0 si no)
    int ula_snow_disabled;

    // ── Estado del borde (OUT 0xFE) ──────────────────────────────────
    uint8_t border_color;
    uint8_t ear_bit;
    uint8_t mic_bit;
    uint8_t keyboard_matrix[8];

    // ── Audio SDL ────────────────────────────────────────────────────
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[882];
    int   audio_pos;

    // ── Reproductores de cinta ───────────────────────────────────────
    TAPPlayer  tap;
    TZXPlayer  tzx;
    TapeSource tape_src;

    bool quit;
    bool turbo_mode;
    int  frame_counter;
} ZXSpectrum;

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