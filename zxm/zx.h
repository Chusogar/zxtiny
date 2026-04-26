#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

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
// Temporización ULA / pantalla completa
//
// El ULA del 48K genera un cuadro de 312 líneas × 224 T-states (a 3.5 MHz).
// 69888 T-states/frame = 312 × 224.
//
// Distribución vertical (líneas de ULA):
//   0..15   → borde superior invisible (retrazado)  [no se muestran]
//  16..63   → borde superior visible  (48 líneas)
//  64..255  → área de imagen          (192 líneas)
// 256..311  → borde inferior visible  (56 líneas en real, aquí 48 para simetría)
//
// Por cada línea horizontal (224 T-states):
//   0.. 15  → borde izquierdo (16 T-states = 2 bytes = 16 píxeles)
//  16..143  → zona de imagen  (128 T-states = 16 bytes = 128 píxeles × 2)
// 144..159  → borde derecho   (16 T-states)
// 160..223  → retrazado horizontal (64 T-states, invisible)
//
// Para simplificar el buffer de borde usamos 1 muestra de color por línea ULA.
// Eso da resolución suficiente para todos los efectos de borde conocidos.
// ---------------------------------------------------------------------------
#define ULA_TSTATES_PER_LINE    224
#define ULA_LINES_PER_FRAME     312
#define ULA_FIRST_PAPER_LINE     64   // Primera línea ULA con imagen
#define ULA_FIRST_VISIBLE_LINE   16   // Primera línea visible (borde sup)
#define ULA_LAST_VISIBLE_LINE   303   // Última línea visible  (borde inf)

// Dimensiones del framebuffer completo (borde + imagen)
// Borde: 48 px arriba, 48 abajo, 48 izquierda, 48 derecha
#define BORDER_LEFT    48
#define BORDER_RIGHT   48
#define BORDER_TOP     48
#define BORDER_BOTTOM  48
#define SCREEN_W      256
#define SCREEN_H      192
#define FULL_W        (BORDER_LEFT + SCREEN_W + BORDER_RIGHT)   // 352
#define FULL_H        (BORDER_TOP  + SCREEN_H + BORDER_BOTTOM)  // 288

// Número de líneas ULA visibles que forman el borde vertical
#define ULA_BORDER_TOP_LINES   48   // líneas ULA 16..63
#define ULA_BORDER_BOT_LINES   48   // líneas ULA 256..303

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
// Buffer de borde por línea ULA
// Almacena el color de borde (0-7) activo al principio de cada una de las
// ULA_LINES_PER_FRAME líneas. spectrum_run_frame() lo rellena; spectrum_render()
// lo consume para pintar el borde con resolución de línea.
// ---------------------------------------------------------------------------
#define BORDER_LINE_BUF  ULA_LINES_PER_FRAME

// ---------------------------------------------------------------------------
// Estructura principal del emulador
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;
    uint8_t memory[65536];

    // Gráficos SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;          // Framebuffer completo FULL_W × FULL_H
    uint32_t framebuffer[FULL_W * FULL_H];

    // Borde por línea ULA
    uint8_t border_lines[BORDER_LINE_BUF]; // color de borde por línea ULA

    // Audio SDL
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[882];
    int   audio_pos;

    // TAP
    TAPPlayer tap;

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
void spectrum_handle_key(ZXSpectrum* spec, SDL_Scancode key, bool pressed);
void spectrum_run_frame(ZXSpectrum* spec);
void spectrum_render(ZXSpectrum* spec);
void spectrum_destroy(ZXSpectrum* spec);

#endif // SPECTRUM_H
