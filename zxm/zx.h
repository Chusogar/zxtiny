#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h" // Core Z80 de jgz80 (o superzazu)

typedef struct {
    z80 cpu;
    uint8_t memory[65536];
    
    // Gráficos (SDL)
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t screen_buffer[256 * 192]; // Pantalla activa del ULA
    
    // Audio (SDL)
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[882]; // 44100 Hz / 50 FPS = 882 samples por frame
    int audio_pos;
    
    bool quit;
    int frame_counter;
} ZXSpectrum;

uint8_t border_color;
    uint8_t keyboard_matrix[8];
    uint8_t ear_bit;
    

// Funciones principales
void spectrum_init(ZXSpectrum* spec);
int spectrum_load_rom(ZXSpectrum* spec, const char* filename);
int spectrum_load_sna(ZXSpectrum* spec, const char* filename);
void spectrum_handle_key(ZXSpectrum* spec, SDL_Scancode key, bool pressed);
void spectrum_run_frame(ZXSpectrum* spec);
void spectrum_render(ZXSpectrum* spec);
void spectrum_destroy(ZXSpectrum* spec);

#endif // SPECTRUM_H