#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h" // Core Z80 de jgz80 (o superzazu)

// ---------------------------------------------------------------------------
// Temporización TAP (en ciclos de T-states a 3.5 MHz)
// ---------------------------------------------------------------------------
#define TAP_PILOT_PULSE     2168   // Duración de cada pulso del tono piloto
#define TAP_PILOT_HEADER    8063   // Nº de pulsos piloto para cabecera (bloque tipo 0)
#define TAP_PILOT_DATA      3223   // Nº de pulsos piloto para datos   (bloque tipo != 0)
#define TAP_SYNC1_PULSE      667   // Primer pulso de sincronismo
#define TAP_SYNC2_PULSE      735   // Segundo pulso de sincronismo
#define TAP_BIT0_PULSE       855   // Duración de cada mitad del pulso "0"
#define TAP_BIT1_PULSE      1710   // Duración de cada mitad del pulso "1"
#define TAP_PAUSE_CYCLES  3500000  // Pausa entre bloques (~1 s a 3.5 MHz)

// ---------------------------------------------------------------------------
// Máquina de estados del reproductor de pulsos TAP
// ---------------------------------------------------------------------------
typedef enum {
    TAP_STATE_IDLE = 0,   // Sin cinta / cinta parada
    TAP_STATE_PILOT,      // Emitiendo tono piloto
    TAP_STATE_SYNC1,      // Primer pulso de sync
    TAP_STATE_SYNC2,      // Segundo pulso de sincronismo
    TAP_STATE_DATA,       // Emitiendo bits de datos
    TAP_STATE_PAUSE       // Pausa entre bloques
} TAPState;

typedef struct {
    // Datos crudos del fichero TAP en memoria
    uint8_t* data;
    uint32_t size;

    // Posición actual dentro del buffer TAP
    uint32_t pos;           // Apunta al inicio del bloque TAP actual
    uint32_t block_len;     // Longitud del bloque actual (bytes de datos)
    uint32_t byte_pos;      // Byte actual dentro del bloque
    uint8_t  bit_mask;      // Máscara del bit actual (0x80..0x01)

    // Máquina de estados
    TAPState state;
    int      pilot_count;   // Pulsos piloto restantes
    int32_t  pulse_cycles;  // T-states que quedan en el pulso actual
    uint8_t  ear;           // Nivel actual del pin EAR (0 ó 1)
    bool     active;        // true = hay cinta reproduciendo
} TAPPlayer;

// ---------------------------------------------------------------------------
// Estructura principal del emulador
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;
    uint8_t memory[65536];

    // Gráficos (SDL)
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t screen_buffer[256 * 192];

    // Audio (SDL)
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[882];   // 44100 Hz / 50 FPS = 882 muestras/frame
    int   audio_pos;

    // Reproductor TAP
    TAPPlayer tap;

    bool quit;
    int  frame_counter;
} ZXSpectrum;

// Variables globales de la ULA (accedidas desde callbacks de I/O)
uint8_t border_color;
uint8_t keyboard_matrix[8];
uint8_t ear_bit;   // EAR de SALIDA (beeper, escritura por el Z80)
uint8_t mic_bit;   // MIC/EAR de ENTRADA (cinta, leído por el Z80)

// ---------------------------------------------------------------------------
// Prototipos
// ---------------------------------------------------------------------------
void spectrum_init(ZXSpectrum* spec);
int  spectrum_load_rom(ZXSpectrum* spec, const char* filename);
int  spectrum_load_sna(ZXSpectrum* spec, const char* filename);
int  spectrum_load_tap(ZXSpectrum* spec, const char* filename);
void spectrum_handle_key(ZXSpectrum* spec, SDL_Scancode key, bool pressed);
void spectrum_run_frame(ZXSpectrum* spec);
void spectrum_render(ZXSpectrum* spec);
void spectrum_destroy(ZXSpectrum* spec);

#endif // SPECTRUM_H
