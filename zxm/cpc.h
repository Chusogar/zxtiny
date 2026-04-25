#ifndef CPC_H
#define CPC_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "z80/jgz80/z80.h"
#include "cpc_fdc.h"

// -----------------------------
// PSG AY-3-8912 (estado + regs)
// -----------------------------
typedef struct {
    uint8_t registers[16];
    uint8_t selected_reg;

    // Tono
    uint16_t tone_period[3];
    uint16_t tone_count[3];
    uint8_t  tone_out[3];

    // Ruido
    uint8_t  noise_period;
    uint16_t noise_count;
    uint32_t noise_lfsr;
    uint8_t  noise_out;

    // Envolvente
    uint16_t env_period;
    uint16_t env_count;
    int      env_step;   // 0..15
    int      env_dir;    // +1/-1
    uint8_t  env_shape;
    uint8_t  env_continue, env_attack, env_alternate, env_hold;
    uint8_t  env_holding;
} PSG;

// PPI 8255
typedef struct {
    uint8_t port_a;
    uint8_t port_b;
    uint8_t port_c;
    uint8_t control;
} PPI;

// CRTC 6845
typedef struct {
    uint8_t registers[18];
    uint8_t selected_reg;
} CRTC;

// -----------------------------
// Máquina CPC
// -----------------------------
typedef struct {
    z80 cpu;

    // RAM 128KB
    uint8_t ram[128 * 1024];

    // ROMs
    uint8_t rom_lower[16384];
    uint8_t rom_upper[16384];
    uint8_t rom_amsdos[16384];

    // Gate Array / paging
    uint8_t palette[17]; // 0..15 + border 16
    uint8_t screen_mode;
    uint8_t rom_lower_enabled;
    uint8_t rom_upper_enabled;
    uint8_t selected_upper_rom;
    uint8_t ram_bank_config;
    uint8_t pen_selected;

    // Periféricos
    PPI ppi;
    PSG psg;
    CRTC crtc;

    // FDC
    FDC fdc;

    uint8_t keyboard_matrix[10];

    // SDL vídeo
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t screen_buffer[800 * 300];

    // -----------------------------
    // SDL audio (cola) + sincronía
    // -----------------------------
    SDL_AudioDeviceID audio_dev;
    int audio_rate;

    uint32_t audio_sample_accum; // acumula cycles*rate
    uint32_t ay_tick_accum;      // acumula ticks AY

    int16_t audio_mixbuf[4096];
    int audio_mixpos;

    // Estado ejecución
    bool quit;
    uint64_t total_cycles;
    int cycles_in_frame;
    int irq_counter;
} AmstradCPC;

// API
void cpc_init(AmstradCPC* cpc);
int  cpc_load_roms(AmstradCPC* hw, const char* firmware_basic_32k, const char* amsdos_16k);
int  cpc_load_sna(AmstradCPC* cpc, const char* filename);
void cpc_handle_key(AmstradCPC* cpc, SDL_Scancode key, bool pressed);
void cpc_run_frame(AmstradCPC* cpc);
void cpc_render(AmstradCPC* cpc);
void cpc_destroy(AmstradCPC* cpc);

#endif