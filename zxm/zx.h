#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "z80/jgz80/z80.h"
#include "tzx.h"

#include "tap_file.h"
// FDC (uPD765) reutilizado del core CPC
#include "cpc_fdc.h"

// Beta 128 Disk Interface (WD1793) para Pentagon/Scorpion
#include "beta128.h"

// ---------------------------------------------------------------------------
// Geometría de pantalla (común 48/128/+3)
// ---------------------------------------------------------------------------
#define SCALE 1
#define BORDER_LEFT 48
#define BORDER_RIGHT 48
#define BORDER_TOP 48
#define BORDER_BOTTOM 48
#define SCREEN_W 256
#define SCREEN_H 192
#define FULL_W (BORDER_LEFT + SCREEN_W + BORDER_RIGHT) // 352
#define FULL_H (BORDER_TOP + SCREEN_H + BORDER_BOTTOM) // 288

// Límites verticales “lógicos” usados por el render actual
#define ULA_FIRST_PAPER_LINE 64
#define ULA_FIRST_VISIBLE_LINE 16

// Máximo ticks/frame para dimensionar tablas (Pentagon: 224*320 = 71680)
#define ULA_MAX_TICKS_PER_FRAME 71680

// CPU / Audio
#define ZX_CPU_CLOCK_HZ 3500000
#define AUDIO_HZ 44100
#define AUDIO_SAMPLES_PER_FRAME (AUDIO_HZ/50) // 882

// AY (Spectrum 128K / +3)
#define AY_CLOCK_HZ 1773400 // ~3.5469MHz/2

// ---------------------------------------------------------------------------
// Selector de fuente de cinta activa
// ---------------------------------------------------------------------------
typedef enum {
    TAPE_NONE = 0,
    TAPE_TAP,
    TAPE_TZX
} TapeSource;

// ---------------------------------------------------------------------------
// Modelo de máquina
// ---------------------------------------------------------------------------
typedef enum {
    ZX_MODEL_48K = 0,
    ZX_MODEL_128K,
    ZX_MODEL_PLUS3,
    ZX_MODEL_PENTAGON,
    ZX_MODEL_PENTA1024,
    ZX_MODEL_SCORPION
} ZXModel;

// ---------------------------------------------------------------------------
// AY-3-8912 (mínimo, suficiente para Spectrum 128/+3)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t regs[16];
    uint8_t sel;

    // Divisores internos
    uint16_t div16;   // acumula ticks para /16
    uint16_t div256;  // acumula ticks para /256

    // Tonos
    uint16_t tone_period[3];
    uint16_t tone_count[3];
    uint8_t  tone_out[3];

    // Ruido
    uint8_t  noise_period;
    uint16_t noise_count;
    uint32_t lfsr;
    uint8_t  noise_out;

    // Envolvente
    uint16_t env_period;
    uint16_t env_count;
    uint8_t  env_shape;
    int8_t   env_step;
    uint8_t  env_vol;
    uint8_t  env_hold;
    uint8_t  env_alt;
    uint8_t  env_attack;
    uint8_t  env_continue;

    // Acumulador de reloj AY (para conversión desde T-states)
    uint32_t tick_accum; // en unidades de ZX_CPU_CLOCK_HZ

    // Nivel último (para mezclar)
    float last_sample;
} AYState;

// ---------------------------------------------------------------------------
// Estructura principal del emulador
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;

    // --- Memoria (bancos 16K)
    uint8_t rom48[16384];
    bool have_rom48;

    // ROM 128K: 2 x 16K
    uint8_t rom128[2][16384];
    bool have_rom128;

    // ROM +3: 4 x 16K
    uint8_t rom_plus3[4][16384];
    bool have_rom_plus3;

    // ROM Beta 128 (TR-DOS): 1 x 16K
    uint8_t rom_beta[16384];
    bool have_rom_beta;

    // ROM Scorpion service: 1 x 16K
    uint8_t rom_scorpion_service[16384];
    bool have_rom_scorpion_service;

    // ROM Scorpion ZS 256: 4 x 16K (scorpa..scorpd.rom) (opcional / futuro)
    uint8_t rom_scorpion[4][16384];
    bool have_rom_scorpion;

    // RAM: 16 bancos x 16K (8 para 128K, 16 para Scorpion 256K)
    uint8_t ram[16][16384];

    // -----------------------------------------------------------------------
    // Tabla de páginas activas AL ESTILO SpectrumFamilyMemory.glPageAt[] (Java)
    //  - 0..15  => índice de RAM (ram[idx])
    //  - 256+N  => “índice ROM” (la traducción a puntero se hace en zx.c)
    // Segmentos:
    //  page_at[0] = 0x0000-0x3FFF
    //  page_at[1] = 0x4000-0x7FFF
    //  page_at[2] = 0x8000-0xBFFF
    //  page_at[3] = 0xC000-0xFFFF
    // -----------------------------------------------------------------------
    uint16_t page_at[4];

    // Mapa actual de páginas 16K (punteros) (0x0000..0xFFFF)
    uint8_t* mem_map[4];

    // Estado de paginación
    ZXModel model;

    // Puerto 0x7FFD (128/+3)
    uint8_t port_7ffd;
    bool paging_lock;
    uint8_t bank_c000;
    uint8_t screen_bank;

    // ROM seleccionada (128: 0/1, +3: 0..3, Scorpion/Penta1024: 0..3)
    uint8_t rom_page;

    // Puerto 0x1FFD (+2A/+3 y Scorpion)
    uint8_t port_1ffd;

    // +3: bit0 => paginación especial
    // Scorpion: bit0 => overlay RAM0 en 0x0000
    bool special_paging;

    // Puntero al banco que usa la ULA como pantalla
    uint8_t* screen_ptr;

    // --- Timing ULA
    int ula_tstates_per_line;  // 48K=224, 128K/+3=228
    int ula_lines_per_frame;   // 48K=312, 128K/+3=311
    int ula_ticks_per_frame;   // 48K=69888, 128K/+3=70908
    int ula_chars_per_line;    // 48K=56, 128K/+3=57

    // SDL vídeo
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t framebuffer[FULL_W * FULL_H];

    // ── ULA: renderizado por T-state ───────────────────────────────
    int ula_count_x;
    int ula_count_y;
    int ula_count_z;
    int ula_shown_x;
    int ula_shown_y;
    int ula_bitmap;
    int ula_attrib;
    int video_y;

    // ── Contención de memoria ─────────────────────────────────────
    uint8_t ula_clash[ULA_MAX_TICKS_PER_FRAME];
    int ula_clash_z;
    int contention_extra;

    // ── Floating bus ──────────────────────────────────────────────
    int ula_bus;

    // ── ULA snow (opcional) ───────────────────────────────────────
    int ula_snow_a;
    int ula_snow_disabled;

    // ── Estado ULA (OUT 0xFE) ─────────────────────────────────────
    uint8_t border_color;
    uint8_t ear_bit;
    uint8_t mic_bit;

    // Input
    uint8_t keyboard_matrix[8];
    uint8_t kempston;

    // Audio SDL
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[AUDIO_SAMPLES_PER_FRAME];
    int audio_pos;

    // AY (128K/+3)
    AYState ay;

    // FDC (+3)
    FDC fdc;

    // Beta 128 (Pentagon/Scorpion):
    // “TR-DOS mapeado en 0x0000-0x3FFF”
    bool trdos_mapped;
	bool beta_active;

    Beta128 beta;

    // Cintas
    TAPPlayer tap;
    TZXPlayer tzx;
    TapeSource tape_src;

    bool quit;
    bool turbo_mode;
    int frame_counter;
} ZXSpectrum;

// API
void spectrum_init(ZXSpectrum* spec, ZXModel model);
void spectrum_destroy(ZXSpectrum* spec);

int spectrum_load_rom48(ZXSpectrum* spec, const char* filename);
int spectrum_load_rom128(ZXSpectrum* spec, const char* filename); // 32K (2x16K)

// +3: 4 ROMs de 16KB (plus3-0..3)
int spectrum_load_rom_plus3_set(ZXSpectrum* spec,
    const char* rom0,
    const char* rom1,
    const char* rom2,
    const char* rom3);

int spectrum_load_rom_beta(ZXSpectrum* spec, const char* filename);
int spectrum_load_rom_scorpion_service(ZXSpectrum* spec, const char* filename);

int spectrum_load_rom_scorpion_set(ZXSpectrum* spec,
    const char* roma,
    const char* romb,
    const char* romc,
    const char* romd);

int spectrum_load_sna(ZXSpectrum* spec, const char* filename);
int spectrum_load_tap(ZXSpectrum* spec, const char* filename);
int spectrum_load_tzx(ZXSpectrum* spec, const char* filename);

void spectrum_tape_start(ZXSpectrum* spec);
void spectrum_handle_key(ZXSpectrum* spec, SDL_Scancode key, bool pressed);
void spectrum_run_frame(ZXSpectrum* spec);
void spectrum_render(ZXSpectrum* spec);

#endif // SPECTRUM_H