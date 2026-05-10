#ifndef ARABIAN_H
#define ARABIAN_H

/*
 * arabian.h  -  Emulador de Arabian (Sun Electronics, 1983)
 *
 * Hardware:
 *   CPU     Z80 @ 4 MHz, modo IM1 (RST 38h cada VBLANK @ 60 Hz)
 *
 *   ROM     32 KB en 0x0000-0x7FFF
 *             ic1rev2.87  0x0000-0x1FFF (8KB)
 *             ic2rev2.88  0x2000-0x3FFF (8KB)
 *             ic3rev2.89  0x4000-0x5FFF (8KB)
 *             ic4rev2.90  0x6000-0x7FFF (8KB)
 *
 *   VRAM    16 KB en 0x8000-0xBFFF (dos planos de bitmap 256x256 4bpp)
 *             La escritura en VRAM actualiza los dos bitmaps en funcion
 *             del registro de plano (spriteram[0] bits 0-3).
 *
 *   RAM     2 KB en 0xD000-0xD7FF
 *
 *   Blitter 0xE000-0xE07F (registros en bloques de 7 bytes):
 *             byte 0: plane mask (bits 0,2 = planos 1,3 del blitter)
 *             byte 1: src_lo
 *             byte 2: src_hi   → src = src_lo | (src_hi<<8) en GFX ROM
 *             byte 3: sy       → coordenada Y destino
 *             byte 4: x>>2     → coordenada X destino (x = byte4<<2)
 *             byte 5: sy_count → height-1
 *             byte 6: sx_count → width-1 (dispara el blit al escribirse)
 *
 *   AY-3-8910 @ 1.5 MHz (puertos I/O 0xC800=control, 0xCA00=write)
 *             Port B (write): bit4=0→leer RAM en D7F0, bit4=1→leer switches
 *
 *   Input   D7F0-D7FF (lectura), 8 registros de 1 byte:
 *             offset 0: IN1  (start1=bit1, start2=bit2, coin3=bit3)
 *             offset 1: IN2  (joystick p1: right=0, left=1, up=2, down=3)
 *             offset 2: IN3  (fire p1: bit0)
 *             offset 3: IN4  (joystick p2: cockail)
 *             offset 4: IN5  (fire p2: bit0)
 *             offset 5: IN6  (unused)
 *             offset 6: clock HI (arabian_clock >> 4)
 *             offset 8: clock LO (arabian_clock & 0x0F)
 *
 *   DSW     0xC000=DSW0 (coin1 bit0, coin2 bit1, test bit2)
 *             0xC200=DSW1 (lives, cabinet, flip, coinage...)
 *
 *   Paleta  32 colores fijos (sin PROM):
 *             colores 0-15: plano bajo (tmpbitmap)
 *             colores 16-31: plano alto (tmpbitmap2)
 *             Transparente: color 8 (indice 8 en cada plano)
 *
 *   Rotacion ROT270 (90 grados CW): visible_area={0,255,11,242}
 *             → pantalla final: 232 x 256 pixels
 *
 *   GFX ROM 8KB x 2 planos (0x0000-0x3FFF y 0x4000-0x7FFF)
 *             Se predecodifican a nibbles en vh_start.
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Mapa de memoria
// ---------------------------------------------------------------------------
#define ARB_ROM_SIZE        0x8000   // 32 KB
#define ARB_VRAM_SIZE       0x4000   // 16 KB (0x8000-0xBFFF)
#define ARB_RAM_SIZE        0x0800   // 2 KB  (0xD000-0xD7FF)
#define ARB_GFX_SIZE        0x8000   // 32 KB total (2 planos x 16KB)
#define ARB_SPRITERAM_SIZE  0x0080   // 128 bytes (blitter regs, 0xE000-0xE07F)

#define ARB_ROM_START       0x0000
#define ARB_ROM_END         0x7FFF
#define ARB_VRAM_START      0x8000
#define ARB_VRAM_END        0xBFFF
#define ARB_DSW0_ADDR       0xC000
#define ARB_DSW1_ADDR       0xC200
#define ARB_RAM_START       0xD000
#define ARB_RAM_END         0xD7FF
#define ARB_INPUT_START     0xD7F0
#define ARB_INPUT_END       0xD7FF
#define ARB_BLITTER_START   0xE000
#define ARB_BLITTER_END     0xE07F

// Puertos I/O
#define ARB_PORT_AY_CTRL    0xC800
#define ARB_PORT_AY_WRITE   0xCA00
#define ARB_PORT_AY_READ    0xCA00

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
// Dos bitmaps internos de 256x256 pixels, cada pixel = indice de paleta (0-15)
// El renderizado final aplica ROT270 y recorte y=[11,242]
// Area visible antes de rotar: x=[0,255], y=[11,242] = 256 x 232 pixels
// Tras ROT270: screen_w=232, screen_h=256
#define ARB_BMP_W           256
#define ARB_BMP_H           256
#define ARB_VIS_Y0          11
#define ARB_VIS_Y1          242
#define ARB_VIS_H           (ARB_VIS_Y1 - ARB_VIS_Y0 + 1)  // 232
#define ARB_SCREEN_W        ARB_BMP_W   // 256 (sin rotacion)
#define ARB_SCREEN_H        ARB_VIS_H   // 232 (sin rotacion, visible y=[11..242])
#define ARB_SCALE           2

// ---------------------------------------------------------------------------
// Paleta fija (32 colores, hardcodeada en vidhrdw/arabian.c de MAME)
// Colores 0-15: plano bajo; 16-31: plano alto
// Indice 8 = color transparente en ambos planos
// ---------------------------------------------------------------------------
#define ARB_NUM_COLORS      32
#define ARB_TRANSPARENT     8   // indice de transparencia en cada plano

// ---------------------------------------------------------------------------
// Temporalizacion
// ---------------------------------------------------------------------------
#define ARB_CPU_CLOCK       4000000   // 4 MHz
#define ARB_FPS             60
#define ARB_CYCLES_PER_FRAME  (ARB_CPU_CLOCK / ARB_FPS)  // 66666

// ---------------------------------------------------------------------------
// Estructura principal
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;

    // Memoria
    uint8_t rom[ARB_ROM_SIZE];
    uint8_t vram[ARB_VRAM_SIZE];         // 0x8000-0xBFFF (no se lee durante el juego)
    uint8_t ram[ARB_RAM_SIZE];           // 0xD000-0xD7FF
    uint8_t spriteram[ARB_SPRITERAM_SIZE]; // 0xE000-0xE07F (blitter regs)

    // GFX ROM decodificada: 0x8000 bytes
    // Formato tras decodificar: cada byte contiene 2 nibbles de 4 bits
    // [offset] = p1|(p2<<4), [offset+0x4000] = p3|(p4<<4)
    uint8_t gfx[ARB_GFX_SIZE];
    bool    have_gfx;

    // Dos bitmaps internos de 256x256, cada pixel = indice de color (0-15)
    // bmp1 = plano bajo (colores 0-15 en paleta)
    // bmp2 = plano alto (colores 16-31 en paleta)
    uint8_t bmp1[ARB_BMP_W * ARB_BMP_H];
    uint8_t bmp2[ARB_BMP_W * ARB_BMP_H];

    // Paleta ARGB computada (32 entradas)
    uint32_t palette[ARB_NUM_COLORS];

    // Framebuffer final (tras ROT270 y recorte): SCREEN_W x SCREEN_H
    uint32_t framebuffer[ARB_SCREEN_W * ARB_SCREEN_H];

    // SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;

    // AY-3-8910 (minimo para port B)
    uint8_t ay_reg;      // registro seleccionado
    uint8_t ay_portB;    // valor escrito en port B

    // Input
    // portB bit4: 0=leer RAM en D7F0, 1=leer switches
    uint8_t in[6];       // in[0]=IN1(start/coin), in[1]=IN2(joy), in[2]=IN3(fire),
                         // in[3]=IN4(joy2), in[4]=IN5(fire2), in[5]=IN6(unused)
    uint8_t dsw0;        // 0xC000
    uint8_t dsw1;        // 0xC200

    // Clock interno (incrementado por interrupcion)
    uint8_t clock_val;

    // Control
    bool quit;
    bool turbo_mode;
    int  frame_counter;

    bool have_rom;

} Arabian;

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void arabian_init(Arabian* a);
void arabian_destroy(Arabian* a);

int  arabian_load_rom(Arabian* a, const char* path);
int  arabian_load_rom_chunk(Arabian* a, const char* path, int offset, int size);
int  arabian_load_gfx(Arabian* a, const char* path, int offset, int size);

void arabian_decode_gfx(Arabian* a);
void arabian_build_palette(Arabian* a);

void arabian_run_frame(Arabian* a);
void arabian_render(Arabian* a);
void arabian_handle_key(Arabian* a, SDL_Scancode sc, bool pressed);

#endif // ARABIAN_H
