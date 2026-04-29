#ifndef C64_H
#define C64_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "m6502/m6502.h"

// ---------------------------------------------------------------------------
// Temporizaci�n PAL (C64 PAL: 985248 Hz, 312 l�neas, 63 ciclos/l�nea)
// ---------------------------------------------------------------------------
#define C64_CPU_FREQ        985248
#define C64_LINES_PER_FRAME 312
#define C64_CYCLES_PER_LINE 63
#define C64_CYCLES_PER_FRAME (C64_LINES_PER_FRAME * C64_CYCLES_PER_LINE) // 19656
#define C64_AUDIO_RATE      44100
#define C64_SAMPLES_PER_FRAME (C64_AUDIO_RATE / 50) // 882

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------
#define C64_SCREEN_W    320
#define C64_SCREEN_H    200
#define C64_BORDER_H     32
#define C64_BORDER_V     36
#define C64_FULL_W      (C64_SCREEN_W + C64_BORDER_H * 2) // 384
#define C64_FULL_H      (C64_SCREEN_H + C64_BORDER_V * 2) // 272
#define C64_SCALE         2

// ---------------------------------------------------------------------------
// VIC-II
// ---------------------------------------------------------------------------
#define VIC_FIRST_VISIBLE_LINE   16
#define VIC_LAST_VISIBLE_LINE   287  // 16 + 272 - 1
#define VIC_FIRST_DISPLAY_LINE   51
#define VIC_LAST_DISPLAY_LINE   250
#define VIC_FIRST_VISIBLE_CYCLE   8
#define VIC_NUM_SPRITES           8

// ---------------------------------------------------------------------------
// SID
// ---------------------------------------------------------------------------
#define SID_NUM_VOICES   3
#define SID_NUM_REGS    29

// Tablas ADSR (rates en ciclos de CPU por incremento/decremento de envolvente)
// El C64 usa una tabla no lineal de 16 valores para attack y 16 para decay/release.

// ---------------------------------------------------------------------------
// CIA
// ---------------------------------------------------------------------------
#define CIA_NUM_REGS    16

// ---------------------------------------------------------------------------
// Paleta de colores C64 (ARGB8888) - colodore.com
// ---------------------------------------------------------------------------
static const uint32_t c64_palette[16] = {
    0xFF000000, // 0  negro
    0xFFFFFFFF, // 1  blanco
    0xFF813338, // 2  rojo
    0xFF75CEC8, // 3  cyan
    0xFF8E3C97, // 4  purpura
    0xFF56AC4D, // 5  verde
    0xFF2E2C9B, // 6  azul
    0xFFEDF171, // 7  amarillo
    0xFF8E5029, // 8  naranja
    0xFF553800, // 9  marron
    0xFFC46C71, // 10 rojo claro
    0xFF4A4A4A, // 11 gris oscuro
    0xFF7B7B7B, // 12 gris medio
    0xFFA9FF9F, // 13 verde claro
    0xFF706DEB, // 14 azul claro
    0xFFB2B2B2, // 15 gris claro
};

// ---------------------------------------------------------------------------
// Voz SID
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t accumulator;   // acumulador de fase 24 bits
    uint32_t shift_reg;     // LFSR para ruido (23 bits)
    uint16_t env_counter;   // contador de envolvente (0-255)
    uint16_t env_rate_counter; // divisor de velocidad de envolvente
    uint8_t  env_state;     // 0=idle, 1=attack, 2=decay, 3=sustain, 4=release
    bool     gate_prev;     // estado anterior del gate para detectar flancos
} SIDVoice;

// ---------------------------------------------------------------------------
// CIA (6526)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t pra, prb;       // port A/B data register
    uint8_t ddra, ddrb;     // port A/B data direction
    uint16_t timer_a;       // timer A counter
    uint16_t timer_b;       // timer B counter
    uint16_t latch_a;       // timer A latch
    uint16_t latch_b;       // timer B latch
    uint8_t  cra, crb;      // control register A/B
    uint8_t  icr;           // interrupt control (pending flags)
    uint8_t  icr_mask;      // interrupt control mask
    uint8_t  sdr;           // serial data register
    uint8_t  tod[4];        // time of day: [0]=10ths, [1]=sec, [2]=min, [3]=hr
    uint8_t  alarm[4];      // alarm time
    bool     tod_running;
    bool     tod_write_alarm; // false=write TOD, true=write alarm
    uint8_t  tod_divider;   // divide 50Hz to 10Hz
} CIA;

// ---------------------------------------------------------------------------
// TAP (cinta)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t pos;
    uint8_t  version;       // 0 o 1
    bool     playing;
    int32_t  pulse_cycles;  // ciclos restantes del pulso actual
    bool     level;         // nivel actual de la se�al
    bool     motor;         // motor encendido (bit 5 de $0001)
    bool     button;        // bot�n PLAY pulsado
} C64Tape;

// ---------------------------------------------------------------------------
// D64 (disco 1541)
// ---------------------------------------------------------------------------
#define D64_SIZE_35   174848
#define D64_SIZE_35E  175531
#define D64_MAX_TRACK     35
#define D64_SECTOR_SIZE  256
#define D64_BAM_TRACK     18

typedef struct {
    uint8_t* data;
    uint32_t size;
    bool     loaded;
} C64Disk;

// ---------------------------------------------------------------------------
// Estructura principal del emulador C64
// ---------------------------------------------------------------------------
typedef struct {
    m6502 cpu;

    // Memoria
    uint8_t ram[65536];
    uint8_t basic_rom[8192];    // $A000-$BFFF
    uint8_t kernal_rom[8192];   // $E000-$FFFF
    uint8_t char_rom[4096];     // $D000-$DFFF (Character ROM)
    uint8_t color_ram[1024];    // $D800-$DBFF (Color RAM, 4 bits usados)

    // 6510 I/O port
    uint8_t port_ddr;   // $0000 data direction
    uint8_t port_dat;   // $0001 data register

    // VIC-II (6569 PAL)
    uint8_t vic_regs[64];
    uint16_t vic_raster;         // l�nea de raster actual
    uint16_t vic_raster_irq;     // l�nea para generar IRQ
    int      vic_cycle;          // ciclo dentro de la l�nea (0..62)
    bool     vic_irq_raster;     // IRQ de raster pendiente
    bool     vic_irq_enabled;    // IRQ de raster habilitada
    bool     vic_display_active; // �rea de display activa
    uint16_t vic_vc;             // video counter
    uint16_t vic_vc_base;        // video counter base
    uint8_t  vic_rc;             // row counter (0-7)
    bool     vic_bad_line;       // l�nea actual es bad line
    uint16_t vic_char_base;      // base de character memory
    uint16_t vic_screen_base;    // base de screen memory
    uint16_t vic_bitmap_base;    // base de bitmap memory
    uint8_t  vic_border_color;
    uint8_t  vic_bg_color[4];    // background colors 0-3
    // Sprite state
    struct {
        uint16_t x;
        uint8_t  y;
        uint8_t  color;
        bool     enabled;
        bool     multicolor;
        bool     expand_x, expand_y;
        bool     priority;       // 0=sprite in front, 1=behind
        uint8_t  pointer;
        uint32_t shift_reg;
        uint8_t  mc_counter;     // multicolor counter
        bool     dma;
        uint8_t  line;           // current line in sprite data
    } sprites[VIC_NUM_SPRITES];
    uint8_t  vic_sprite_mc[2];   // sprite multicolor 0,1

    // SID (6581)
    uint8_t  sid_regs[SID_NUM_REGS];
    SIDVoice sid_voice[SID_NUM_VOICES];
    uint8_t  sid_volume;
    uint8_t  sid_filter_mode;
    uint8_t  sid_filter_route;
    uint16_t sid_filter_cutoff;
    uint8_t  sid_filter_resonance;

    // CIA 1 & 2
    CIA cia[2];

    // Teclado (matriz 8�8)
    uint8_t keyboard_matrix[8]; // filas, bits activos = 0 (pulsado)

    // Joystick port 2 (bits activos = 0)
    uint8_t joystick;

    // SDL v�deo
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t framebuffer[C64_FULL_W * C64_FULL_H];

    // SDL audio
    SDL_AudioDeviceID audio_dev;
    float audio_buffer[C64_SAMPLES_PER_FRAME * 2]; // margen extra
    int   audio_pos;

    // Tape
    C64Tape tape;

    // Disk
    C64Disk disk;

    // Estado
    bool quit;
    bool turbo_mode;
    // PRG pendiente (carga diferida para que BASIC se inicialice primero)
    char pending_prg[512];
    bool prg_pending;
	char pending_tap[512];
    bool tap_pending;
    int  frame_counter;
} C64;

// Prototipos p�blicos
void c64_init(C64* c);
void c64_destroy(C64* c);
int  c64_load_roms(C64* c, const char* dir);
int  c64_load_tap(C64* c, const char* path);
int  c64_load_d64(C64* c, const char* path);
int  c64_load_prg(C64* c, const char* path);
void c64_run_frame(C64* c);
void c64_render(C64* c);

#endif // C64_H