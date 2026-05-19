#ifndef SMS_H
#define SMS_H

/*
 * sms.h  -  Emulador de Sega Master System (SMS / Mark III)
 *
 * Hardware:
 *   CPU   : Z80 @ 3.579545 MHz (NTSC: 53.203MHz / 15)
 *   VDP   : Custom derivado de TMS9918A, 16KB VRAM, 32 bytes CRAM
 *            Modo 4 (todos los juegos excepto F-16 Fighter)
 *            256x192 (small), 256x224 (medium), 256x240 (large)
 *   Sound : SN76489 PSG (no emulado)
 *   RAM   : 8 KB en 0xC000-0xDFFF (espejada en 0xE000-0xFFFF)
 *   ROM   : hasta 512 KB con mapper Sega en 0x0000-0xBFFF
 *
 * Mapa de memoria:
 *   0x0000-0x03FF  ROM banco 0 (primeros 1KB, fijos, no paginables)
 *   0x0400-0x3FFF  ROM slot 0 (16KB-1KB), banco seleccionado por 0xFFFC
 *   0x4000-0x7FFF  ROM slot 1 (16KB), banco seleccionado por 0xFFFD
 *   0x8000-0xBFFF  ROM slot 2 (16KB), banco seleccionado por 0xFFFE
 *   0xC000-0xDFFF  RAM (8KB)
 *   0xE000-0xFFFF  RAM espejo
 *   0xFFFC         Page control register
 *   0xFFFC-0xFFFF  Bank select registers
 *
 * Puertos I/O:
 *   0x7E (R) : VDP V-counter
 *   0x7F (R) : VDP H-counter
 *   0x7E (W) : SN76489 PSG data
 *   0x7F (W) : SN76489 PSG data (espejo)
 *   0xBE (R) : VDP data port (lee read buffer, incrementa addr)
 *   0xBE (W) : VDP data port (escribe en VRAM/CRAM)
 *   0xBF (R) : VDP status register
 *   0xBF (W) : VDP control port (control word)
 *   0xBD (W) : espejo de 0xBF
 *   0xDC (R) : Joypad 1 (active-low)
 *   0xDD (R) : Joypad 2 + misc (active-low)
 *   0xC0 (R) : espejo de 0xDC
 *   0xC1 (R) : espejo de 0xDD
 *
 * VDP modo 4:
 *   VRAM 0x0000-0x3FFF (16KB)
 *   Name table: default 0x3800 (controlado por reg 2)
 *   Sprite attr table: default 0x3F00 (controlado por reg 5)
 *   Tile patterns: 0x0000-0x37FF (tiles 0-447)
 *   Cada tile: 32 bytes (8 filas x 4 bytes/fila, 4bpp, 2 pixels por word)
 *   Paleta: 32 bytes CRAM (2 paletas de 16 colores, formato SMS: 2 bits por canal)
 *   Sprites: 64 sprites 8x8 o 8x16
 *
 * Timing NTSC:
 *   CPU clock   = 3.579545 MHz
 *   VDP clock   = 5.369318 MHz (= CPU * 3 / 2)
 *   FPS         = 59.9227 Hz
 *   Scanlines   = 262 por frame
 *   H cycles    = 684 ticks de maquina por scanline
 *   Z80 cycles  = 228 por scanline (= 684 / 3)
 *
 * Interrupciones:
 *   IRQ (Z80 /INT) = VSync (fin de area activa) si reg1.bit5 activo
 *                  + Line interrupt si reg0.bit4 activo
 *   NMI (Z80 /NMI) = boton PAUSE (no emulado aqui)
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Constantes
// ---------------------------------------------------------------------------

// Timing NTSC
#define SMS_CPU_CLOCK       3579545   // 3.579545 MHz
#define SMS_FPS             60
#define SMS_SCANLINES       262       // total por frame (NTSC)
#define SMS_LINES_ACTIVE    192       // resolución small por defecto
#define SMS_Z80_PER_LINE    228       // ciclos Z80 por scanline (228.5...)
#define SMS_CYCLES_PER_FRAME (SMS_Z80_PER_LINE * SMS_SCANLINES) // 59736

// VCounter jump points (NTSC small 192)
#define SMS_VCNT_JUMP_FROM  0xDA     // cuando vcounter llega aqui...
#define SMS_VCNT_JUMP_TO    0xD5     // ...salta a aqui

// Video
#define SMS_SCREEN_W        256
#define SMS_SCREEN_H_SMALL  192
#define SMS_SCREEN_H_MED    224
#define SMS_SCREEN_H_LARGE  240
#define SMS_SCREEN_H        SMS_SCREEN_H_SMALL   // resolución por defecto
#define SMS_SCALE           2

// Memoria
#define SMS_ROM_MAXSIZE     (512*1024)  // 512 KB
#define SMS_RAM_SIZE        0x2000      // 8 KB
#define SMS_VRAM_SIZE       0x4000      // 16 KB
#define SMS_CRAM_SIZE       32          // 32 bytes (2 paletas x 16 colores)
#define SMS_ROM_SLOT_SIZE   0x4000      // 16 KB por slot
#define SMS_FIRST_KB        0x0400      // primer 1KB siempre fijo

// Joypad bits (0xDC): active-low
#define SMS_P1_UP           0x01
#define SMS_P1_DOWN         0x02
#define SMS_P1_LEFT         0x04
#define SMS_P1_RIGHT        0x08
#define SMS_P1_FIRE_A       0x10
#define SMS_P1_FIRE_B       0x20
// bits 6-7 son P2_UP y P2_DOWN en 0xDC
#define SMS_P2_UP           0x40
#define SMS_P2_DOWN         0x80

// 0xDD: active-low
#define SMS_P2_LEFT         0x01
#define SMS_P2_RIGHT        0x02
#define SMS_P2_FIRE_A       0x04
#define SMS_P2_FIRE_B       0x08
#define SMS_RESET_BTN       0x10   // bit 4 = boton RESET

// ---------------------------------------------------------------------------
// VDP
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  vram[SMS_VRAM_SIZE];   // 16 KB
    uint8_t  cram[SMS_CRAM_SIZE];   // 32 bytes color RAM

    // Control word (2 bytes escritos secuencialmente en 0xBF)
    uint16_t ctrl_word;
    bool     ctrl_second;           // esperando segundo byte del ctrl word

    // Registros de control (0-10)
    uint8_t  reg[11];

    // Status register: bit7=VSync IRQ pending, bit6=sprite overflow, bit5=collision
    uint8_t  status;

    // Read buffer (leido con port 0xBE)
    uint8_t  read_buf;

    // Contadores
    uint8_t  vcounter;             // linea actual (wraps)
    uint8_t  hcounter;             // columna actual
    bool     vcnt_jumped;          // flag para el jump del vcounter

    // Interrupt state
    bool     irq_pending;          // solicitud de IRQ a la CPU
    int8_t   line_counter;         // contador de linea (reg 0xA)
    bool     line_irq;             // IRQ de linea pendiente

    // Scroll interno (se actualiza solo fuera de la zona activa)
    uint8_t  vscroll;              // Y scroll interno

    // Altura activa actual
    int      height;               // 192, 224 o 240

    // Framebuffer de scanlines (solo la zona activa)
    uint32_t framebuffer[SMS_SCREEN_W * SMS_SCREEN_H_LARGE];

    // Paleta ARGB (32 entradas)
    uint32_t palette[SMS_CRAM_SIZE];

} VDP;

// ---------------------------------------------------------------------------
// Estructura principal SMS
// ---------------------------------------------------------------------------
typedef struct {
    z80     cpu;

    // ROM (hasta 512KB)
    uint8_t* rom;
    int      rom_size;

    // RAM (8KB, espejada)
    uint8_t  ram[SMS_RAM_SIZE];

    // Mapper Sega: 3 slots de 16KB
    int      slot[3];        // banco activo en cada slot (indice de 16KB)
    int      num_banks;      // total de bancos de 16KB en la ROM

    // VDP
    VDP      vdp;

    // SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;   // SMS_SCREEN_W x SMS_SCREEN_H (variable)
    int           tex_h;     // altura actual de la textura

    // Input
    uint8_t  port_dc;        // joypad 1 + P2 up/down (active-low, 0xFF = nada)
    uint8_t  port_dd;        // joypad 2 + misc (active-low)

    // Control
    bool     quit;
    bool     paused;
    int      frame_counter;

    // Nombre ROM cargada
    char     rom_name[256];

} SMS;

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void sms_init(SMS* s);
void sms_destroy(SMS* s);
int  sms_load_rom(SMS* s, const char* path);
void sms_run_frame(SMS* s);
void sms_render(SMS* s);
void sms_handle_key(SMS* s, SDL_Scancode sc, bool pressed);

#endif // SMS_H
