/*
 * arabian.c  -  Emulador de Arabian (Sun Electronics, 1983)
 *
 * Estructura y estilo basados en zx.c / minivadr.c / phoenix.c (zxtiny).
 * Driver original: MAME 0.37b7 drivers/arabian.c + vidhrdw/arabian.c
 *
 * La parte mas compleja es el sistema de video:
 *   - Dos bitmaps internos de 256x256 (plano bajo y plano alto)
 *   - Blitter de hardware activado por escrituras en 0xE000-0xE07F
 *   - Escrituras directas a VRAM (0x8000-0xBFFF) actualizan los bitmaps
 *     via arabian_videoram_w, que mezcla bits de los planos en funcion
 *     de spriteram[0] (mascara de plano activa)
 *   - Paleta fija de 32 colores hardcodeada (16 por plano)
 *   - ROT270 y recorte y=[11,242]
 */

#include "arabian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Paleta fija (de arabian_vh_convert_color_prom en MAME)
// ---------------------------------------------------------------------------

static void build_palette(Arabian* a) {
    // Colores 0-15: plano bajo
    // MAME: on = (i&8)?0x80:0xFF; R=(i&4)?0xFF:0; G=(i&2)?on:0; B=(i&1)?on:0
    for (int i = 0; i < 16; i++) {
        uint8_t on = (i & 0x08) ? 0x80 : 0xFF;
        uint8_t r  = (i & 0x04) ? 0xFF : 0x00;
        uint8_t g  = (i & 0x02) ? on   : 0x00;
        uint8_t b  = (i & 0x01) ? on   : 0x00;
        a->palette[i] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
    }

    // Colores 16-31: plano alto (hardcodeados en MAME "to match the screenshot")
    // Reproduzco la tabla exacta del vidhrdw/arabian.c de MAME:
    static const uint8_t hi_pal[16][3] = {
        {0x00,0x00,0x00}, // 16: 0000 negro
        {0x00,0xFF,0x00}, // 17: 0001 verde
        {0x00,0xFF,0x00}, // 18: 0010 verde
        {0x00,0xFF,0x00}, // 19: 0011 verde
        {0xFF,0x00,0x00}, // 20: 0100 rojo
        {0xFF,0xFF,0x00}, // 21: 0101 amarillo
        {0xFF,0xFF,0x00}, // 22: 0110 amarillo
        {0xFF,0xFF,0x00}, // 23: 0111 amarillo
        {0x00,0x00,0x00}, // 24: 1000 negro (transparente para bmp2)
        {0xFF,0xFF,0x00}, // 25: 1001 amarillo
        {0xFF,0x80,0x00}, // 26: 1010 naranja
        {0x00,0xFF,0x00}, // 27: 1011 verde
        {0xFF,0x00,0x00}, // 28: 1100 rojo
        {0xFF,0xFF,0x00}, // 29: 1101 amarillo
        {0xFF,0x80,0x00}, // 30: 1110 naranja
        {0xFF,0xFF,0x00}, // 31: 1111 amarillo
    };
    for (int i = 0; i < 16; i++) {
        a->palette[16 + i] = 0xFF000000u
            | ((uint32_t)hi_pal[i][0] << 16)
            | ((uint32_t)hi_pal[i][1] <<  8)
            |  (uint32_t)hi_pal[i][2];
    }
}

// ---------------------------------------------------------------------------
// Decodificacion de GFX ROM (arabian_vh_start en MAME)
// Formato original: cada byte tiene 4 pixels entrelazados (bits 0,1,2,3 y 4,5,6,7)
// Tras decodificar: [offset] = p1|(p2<<4), [offset+0x4000] = p3|(p4<<4)
// donde p1..p4 son nibbles de 4 bits (un pixel de 4bpp cada uno).
// La formula MAME (para cada offset 0..0x3FFF):
//   v1 = gfx[offset], v2 = gfx[offset+0x4000]
//   p1 = (v1&1) | ((v1&0x10)>>3) | ((v2&1)<<2)   | ((v2&0x10)>>1)
//   (luego v1>>=1, v2>>=1 para p2, p3, p4)
// ---------------------------------------------------------------------------

void arabian_decode_gfx(Arabian* a) {
    for (int offs = 0; offs < 0x4000; offs++) {
        int v1 = a->gfx[offs];
        int v2 = a->gfx[offs + 0x4000];
        int p1, p2, p3, p4;

        p1 = (v1 & 0x01) | ((v1 & 0x10) >> 3) | ((v2 & 0x01) << 2) | ((v2 & 0x10) >> 1);
        v1 >>= 1; v2 >>= 1;
        p2 = (v1 & 0x01) | ((v1 & 0x10) >> 3) | ((v2 & 0x01) << 2) | ((v2 & 0x10) >> 1);
        v1 >>= 1; v2 >>= 1;
        p3 = (v1 & 0x01) | ((v1 & 0x10) >> 3) | ((v2 & 0x01) << 2) | ((v2 & 0x10) >> 1);
        v1 >>= 1; v2 >>= 1;
        p4 = (v1 & 0x01) | ((v1 & 0x10) >> 3) | ((v2 & 0x01) << 2) | ((v2 & 0x10) >> 1);

        a->gfx[offs]          = (uint8_t)(p1 | (p2 << 4));
        a->gfx[offs + 0x4000] = (uint8_t)(p3 | (p4 << 4));
    }
}

void arabian_build_palette(Arabian* a) {
    build_palette(a);
}

// ---------------------------------------------------------------------------
// Blitter: blit_byte  (MAME: blit_byte inline)
// Escribe 4 pixels en los bitmaps en la posicion (x, y) con dx=1, dy=0
// (orientacion ROT270: swap XY, flip X → dx=0, dy=-1 efectivo en logico)
//
// Arabian usa ROT270 = SWAP_XY + FLIP_X.
// En MAME con esa orientacion, blit_byte transforma:
//   t=x; x=y; y=t; t=dx; dx=dy; dy=t;  → dx=0, dy=1 (antes: dx=1,dy=0)
//   flip_x: x = x^0xFF, dx = -dx  → dy=-1 ahora
// Resultado: los 4 pixels se escriben verticalmente hacia arriba.
// Pero en nuestro bitmap interno (sin rotar) aplicamos la transformacion
// inversa al renderizar, asi que escribimos en coordenadas logicas directas
// con la transformacion ya aplicada al blit:
//   bmp[y][x], bmp[y-1][x], bmp[y-2][x], bmp[y-3][x]
// El driver escribe p4 en (x,y), p3 en (x,y+1), p2 en (x,y+2), p1 en (x,y+3)
// Pero con ROT270: se convierte en columnas.
// Para simplificar reproducimos la logica MAME en coordenadas de bitmap
// ya transformadas (como si fuera un bitmap 256x256 sin rotar).
// ---------------------------------------------------------------------------

static void blit_byte_to_bmp(uint8_t* bmp, int x, int y, int p1, int p2, int p3, int p4, int transparent) {
    // Escribe 4 pixels verticalmente: y, y+1, y+2, y+3 en columna x
    // (ya en coordenadas de bitmap logico)
    struct { int py; int pix; } pxs[4] = {{y,p4},{y+1,p3},{y+2,p2},{y+3,p1}};
    for (int i = 0; i < 4; i++) {
        int px = x, py = pxs[i].py;
        if (px < 0 || px >= ARB_BMP_W || py < 0 || py >= ARB_BMP_H) continue;
        if (pxs[i].pix == transparent) continue;
        bmp[py * ARB_BMP_W + px] = (uint8_t)pxs[i].pix;
    }
}

// blit_byte: escribe un byte de GFX en las posiciones correctas de los bitmaps
// Coordenadas en espacio de bitmap logico (256x256, sin rotar).
// Con ROT270: x_logico = y_hardware, y_logico = 255 - x_hardware
// El driver MAME pasa x e y en coordenadas hardware; nosotros los almacenamos
// en coordenadas logicas directas para facilitar el renderizado.
static void blit_byte(Arabian* a, uint8_t x_hw, uint8_t y_hw, int val, int val2, uint8_t plane) {
    int p4 =  val        & 0x0F;
    int p3 = (val  >> 4) & 0x0F;
    int p2 =  val2       & 0x0F;
    int p1 = (val2 >> 4) & 0x0F;

    // ROT270 (SWAP_XY + FLIP_X) → transformacion al espacio logico:
    // x_log = y_hw, y_log = 255 - x_hw
    // Los 4 pixels ocupan y_log, y_log-1, y_log-2, y_log-3
    // (ya que dx se convierte en dy=-1 tras la transformacion)
    int x_log = y_hw;
    int y_log = 255 - (int)x_hw;

    if (plane & 0x01) {
        // bmp1: pixels con valor != ARB_TRANSPARENT (8)
        int ys[4] = {y_log, y_log-1, y_log-2, y_log-3};
        int ps[4] = {p4, p3, p2, p1};
        for (int i = 0; i < 4; i++) {
            if (x_log < 0 || x_log >= ARB_BMP_W) continue;
            if (ys[i] < 0 || ys[i] >= ARB_BMP_H) continue;
            if (ps[i] == ARB_TRANSPARENT) continue;
            a->bmp1[ys[i] * ARB_BMP_W + x_log] = (uint8_t)ps[i];
        }
    }
    if (plane & 0x04) {
        // bmp2: pixels con valor != ARB_TRANSPARENT (8). Se renderiza como palette[16+idx]
        int ys[4] = {y_log, y_log-1, y_log-2, y_log-3};
        int ps[4] = {p4, p3, p2, p1};
        for (int i = 0; i < 4; i++) {
            if (x_log < 0 || x_log >= ARB_BMP_W) continue;
            if (ys[i] < 0 || ys[i] >= ARB_BMP_H) continue;
            if (ps[i] == ARB_TRANSPARENT) continue;
            a->bmp2[ys[i] * ARB_BMP_W + x_log] = (uint8_t)ps[i];
        }
    }
    (void)blit_byte_to_bmp;  // silenciar warning de funcion no usada
}

// ---------------------------------------------------------------------------
// arabian_blit_area (MAME: arabian_blit_area)
// ---------------------------------------------------------------------------

static void arabian_blit_area(Arabian* a, uint8_t plane, uint16_t src,
                               uint8_t x, uint8_t y, uint8_t sx, uint8_t sy) {
    for (int i = 0; i <= sx; i++, x += 4) {
        for (int j = 0; j <= sy; j++) {
            uint16_t s = src + (uint16_t)(i * (sy + 1) + j);
            if (s >= 0x4000) s &= 0x3FFF;
            blit_byte(a, x, (uint8_t)(y + j),
                      a->gfx[s], a->gfx[s + 0x4000], plane);
        }
    }
}

// ---------------------------------------------------------------------------
// arabian_videoram_w (MAME: arabian_videoram_w)
// Escritura directa en VRAM → actualiza bitmaps con mascara de plano
// de spriteram[0] bits 0-3.
// offset: byte escrito en 0x8000-0xBFFF → offset = addr - 0x8000
// x_hw = (offset>>8)<<2, y_hw = offset&0xFF
// Los 4 planos (bits 0-3 de spriteram[0]) seleccionan qué bits
// de cada pixel se actualizan en bmp1/bmp2.
// ---------------------------------------------------------------------------

static void arabian_videoram_w(Arabian* a, uint16_t offset, uint8_t data) {
    a->vram[offset] = data;

    int plane1 = a->spriteram[0] & 0x01;
    int plane2 = a->spriteram[0] & 0x02;
    int plane3 = a->spriteram[0] & 0x04;
    int plane4 = a->spriteram[0] & 0x08;

    uint8_t x_hw = (uint8_t)((offset >> 8) << 2);
    uint8_t y_hw = (uint8_t)(offset & 0xFF);

    // Transformacion ROT270 al espacio logico
    int x_log = y_hw;
    int y_log = 255 - (int)x_hw;

    // 4 posiciones verticales (pixels consecutivos en coordenadas logicas)
    int ys[4] = { y_log, y_log-1, y_log-2, y_log-3 };
    // Bits de datos para cada pixel (4 pixels por byte):
    // pixel0: bits 0,4; pixel1: bits 1,5; pixel2: bits 2,6; pixel3: bits 3,7
    int bits_lo[4] = { data & 0x01, (data>>1) & 0x01, (data>>2) & 0x01, (data>>3) & 0x01 };
    int bits_hi[4] = { (data>>4)&0x01, (data>>5)&0x01, (data>>6)&0x01, (data>>7)&0x01 };

    for (int i = 0; i < 4; i++) {
        if (x_log < 0 || x_log >= ARB_BMP_W) continue;
        if (ys[i] < 0 || ys[i] >= ARB_BMP_H) continue;

        // bmp1: plano 1 modifica bits 2-3 del pixel; plano 2 modifica bits 0-1
        if (plane1) {
            uint8_t* p = &a->bmp1[ys[i] * ARB_BMP_W + x_log];
            *p = (*p & 0xF3) | (bits_hi[i] ? 8 : 0) | (bits_lo[i] ? 4 : 0);
        }
        if (plane2) {
            uint8_t* p = &a->bmp1[ys[i] * ARB_BMP_W + x_log];
            *p = (*p & 0xFC) | (bits_hi[i] ? 2 : 0) | (bits_lo[i] ? 1 : 0);
        }
        // bmp2: plano 3 modifica bits 2-3; plano 4 modifica bits 0-1
        // El valor base en bmp2 es el indice en [0..15]; se muestra como palette[16+idx]
        if (plane3) {
            uint8_t* p = &a->bmp2[ys[i] * ARB_BMP_W + x_log];
            uint8_t idx = *p; // puede ser 0..15 o 16..31
            if (idx >= 16) idx -= 16;
            idx = (idx & 0xF3) | (bits_hi[i] ? 8 : 0) | (bits_lo[i] ? 4 : 0);
            *p = idx;  // guardamos 0..15 (se suma 16 al renderizar si no transparente)
        }
        if (plane4) {
            uint8_t* p = &a->bmp2[ys[i] * ARB_BMP_W + x_log];
            uint8_t idx = *p;
            if (idx >= 16) idx -= 16;
            idx = (idx & 0xFC) | (bits_hi[i] ? 2 : 0) | (bits_lo[i] ? 1 : 0);
            *p = idx;
        }
    }
}

// ---------------------------------------------------------------------------
// Blitter handler: arabian_blitter_w (MAME: arabian_blitter_w)
// Se activa cuando el offset es multiplo de 7 (offset & 7 == 6)
// ---------------------------------------------------------------------------

static void do_blitter(Arabian* a, int base) {
    uint8_t plane = a->spriteram[base + 0];
    uint16_t src  = (uint16_t)(a->spriteram[base + 1]) |
                    ((uint16_t)(a->spriteram[base + 2]) << 8);
    uint8_t y     = a->spriteram[base + 3];
    uint8_t x     = (uint8_t)(a->spriteram[base + 4] << 2);
    uint8_t sy    = a->spriteram[base + 5];
    uint8_t sx    = a->spriteram[base + 6];

    src &= 0x3FFF;
    arabian_blit_area(a, plane, src, x, y, sx, sy);
}

// ---------------------------------------------------------------------------
// Callbacks Z80
// ---------------------------------------------------------------------------

static uint8_t mem_read(void* userdata, uint16_t addr) {
    Arabian* a = (Arabian*)userdata;

    if (addr <= ARB_ROM_END)
        return a->rom[addr];

    if (addr >= ARB_VRAM_START && addr <= ARB_VRAM_END)
        return a->vram[addr - ARB_VRAM_START];

    if (addr == ARB_DSW0_ADDR)
        return a->dsw0;

    if (addr == ARB_DSW1_ADDR)
        return a->dsw1;

    if (addr >= ARB_RAM_START && addr <= ARB_RAM_END) {
        int off = addr - ARB_RAM_START;
        // D7F0-D7FF: lectura de input o RAM segun portB bit4
        if (addr >= ARB_INPUT_START && addr <= ARB_INPUT_END) {
            int idx = addr - ARB_INPUT_START;
            if (a->ay_portB & 0x10) {
                // Leer switches
                switch (idx) {
                case 0: return a->in[0];  // IN1: start/coin
                case 1: return a->in[1];  // IN2: joystick p1
                case 2: return a->in[2];  // IN3: fire p1
                case 3: return a->in[3];  // IN4: joystick p2
                case 4: return a->in[4];  // IN5: fire p2
                case 5: return a->in[5];  // IN6: unused
                case 6: return (uint8_t)(a->clock_val >> 4);
                case 8: return (uint8_t)(a->clock_val & 0x0F);
                default: return 0x00;
                }
            } else {
                // Leer RAM
                return a->ram[off];
            }
        }
        return a->ram[off];
    }

    return 0xFF;
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    Arabian* a = (Arabian*)userdata;

    if (addr <= ARB_ROM_END) return;  // ROM: solo lectura

    if (addr >= ARB_VRAM_START && addr <= ARB_VRAM_END) {
        arabian_videoram_w(a, addr - ARB_VRAM_START, val);
        return;
    }

    if (addr >= ARB_RAM_START && addr <= ARB_RAM_END) {
        a->ram[addr - ARB_RAM_START] = val;
        return;
    }

    if (addr >= ARB_BLITTER_START && addr <= ARB_BLITTER_END) {
        int off = addr - ARB_BLITTER_START;
        a->spriteram[off] = val;
        // El blit se ejecuta cuando se escribe el byte 6 de cada bloque
        if ((off & 0x07) == 6) {
            do_blitter(a, off - 6);
        }
        return;
    }
}

static uint8_t port_in(z80* z, uint16_t port) {
    Arabian* a = (Arabian*)z->userdata;
    uint16_t hi = port & 0xFF00;

    // Lectura de datos del AY (muy basico). Si el juego lee port B (reg 0x0F), devolvemos su latch.
    if (hi == (ARB_PORT_AY_READ & 0xFF00)) {
        if (a->ay_reg == 0x0F) return a->ay_portB;
        return 0xFF;
    }

    return 0xFF;
}


static void port_out(z80* z, uint16_t port, uint8_t val) {
    Arabian* a = (Arabian*)z->userdata;

    // AY-3-8910 mapeado en memoria: 0xC800 = control (seleccion de registro), 0xCA00 = data write.
    // IMPORTANTE: ambos tienen byte bajo 0x00, asi que hay que mirar el byte alto.
    uint16_t hi = port & 0xFF00;

    if (hi == (ARB_PORT_AY_CTRL & 0xFF00)) {
        a->ay_reg = val;
        return;
    }
    if (hi == (ARB_PORT_AY_WRITE & 0xFF00)) {
        // Solo nos interesa el port B del AY (registro 0x0F).
        if (a->ay_reg == 0x0F) a->ay_portB = val;
        return;
    }
}


// ---------------------------------------------------------------------------
// arabian_init
// ---------------------------------------------------------------------------

void arabian_init(Arabian* a) {
    memset(a, 0, sizeof(*a));

    // Input: active-high → todo a 0 (sin pulsar)
    memset(a->in, 0x00, sizeof(a->in));

    // DSW1: 3 vidas, upright, flip off, carry bowls yes, 1c/1c
    a->dsw0 = 0x00;
    a->dsw1 = 0x06;  // bit1=upright, bit2=flip_off

    // Bitmaps: indice 8 = transparente → inicializar con 8
    memset(a->bmp1, ARB_TRANSPARENT, sizeof(a->bmp1));
    memset(a->bmp2, ARB_TRANSPARENT, sizeof(a->bmp2));

    build_palette(a);

    // CPU
    z80_init(&a->cpu);
    a->cpu.userdata   = a;
    a->cpu.read_byte  = mem_read;
    a->cpu.write_byte = mem_write;
    a->cpu.port_in    = port_in;
    a->cpu.port_out   = port_out;

    // SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    a->window = SDL_CreateWindow(
        "Arabian (Sun Electronics, 1983)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ARB_SCREEN_W * ARB_SCALE,
        ARB_SCREEN_H * ARB_SCALE,
        SDL_WINDOW_SHOWN
    );

    a->renderer = SDL_CreateRenderer(
        a->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    a->texture = SDL_CreateTexture(
        a->renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        ARB_SCREEN_W,
        ARB_SCREEN_H
    );

    SDL_RenderSetLogicalSize(
        a->renderer,
        ARB_SCREEN_W * ARB_SCALE,
        ARB_SCREEN_H * ARB_SCALE
    );
}

// ---------------------------------------------------------------------------
// arabian_destroy
// ---------------------------------------------------------------------------

void arabian_destroy(Arabian* a) {
    if (a->texture)  SDL_DestroyTexture(a->texture);
    if (a->renderer) SDL_DestroyRenderer(a->renderer);
    if (a->window)   SDL_DestroyWindow(a->window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Carga de ROMs
// ---------------------------------------------------------------------------

static int load_file(uint8_t* dst, int max_size, const char* path, int offset, int len) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    int to_read = (len > 0) ? len : (int)fsz;
    if (offset + to_read > max_size) {
        fprintf(stderr, "[ROM] '%s': desbordamiento\n", path);
        fclose(f); return -1;
    }
    int n = (int)fread(dst + offset, 1, (size_t)to_read, f);
    fclose(f);
    if (n != to_read) { fprintf(stderr, "[ROM] '%s': leidos %d de %d\n", path, n, to_read); return -1; }
    printf("[ROM] '%s' -> offset 0x%04X (%d bytes)\n", path, offset, n);
    return 0;
}

int arabian_load_rom(Arabian* a, const char* path) {
    int r = load_file(a->rom, ARB_ROM_SIZE, path, 0, 0);
    if (r == 0) a->have_rom = true;
    return r;
}

int arabian_load_rom_chunk(Arabian* a, const char* path, int offset, int size) {
    return load_file(a->rom, ARB_ROM_SIZE, path, offset, size);
}

int arabian_load_gfx(Arabian* a, const char* path, int offset, int size) {
    return load_file(a->gfx, ARB_GFX_SIZE, path, offset, size);
}

// ---------------------------------------------------------------------------
// arabian_run_frame
// IRQ IM1 (RST 38h) una vez por VBLANK @ 60 Hz
// ---------------------------------------------------------------------------

void arabian_run_frame(Arabian* a) {
    a->clock_val++;
    z80_step_n(&a->cpu, ARB_CYCLES_PER_FRAME);
    z80_pulse_irq(&a->cpu, 0xFF);
    a->frame_counter++;
}

// ---------------------------------------------------------------------------
// arabian_render
// Composicion: bmp2 (plano alto) sobre bmp1 (plano bajo),
// con transparencia en indice ARB_TRANSPARENT (8) de cada plano.
// Luego aplica ROT270 y recorte y=[11,242] → framebuffer SCREEN_W x SCREEN_H
//
// ROT270 (CW): screen(x,y) = log(y, SCREEN_W-1-x)
//   screen_x in [0, SCREEN_W-1] = [0, 231]
//   screen_y in [0, SCREEN_H-1] = [0, 255]
//   log_x = screen_y   (0..255)
//   log_y = (ARB_SCREEN_W-1) - screen_x  (231..0)
// Recorte vertical en coordenadas logicas: y_log in [VIS_Y0, VIS_Y1] = [11, 242]
// Esto equivale a: screen_x en [0, VIS_H-1] cuando log_y in [VIS_Y0, VIS_Y1]
// log_y = SCREEN_W-1-screen_x → screen_x = SCREEN_W-1-log_y
// Recorte: log_y in [11,242] → screen_x in [SCREEN_W-1-242, SCREEN_W-1-11]
//                                          = [13, 220] en coordenadas de bitmap
// Sin embargo, para simplificar mapeamos directamente:
//   Para cada (sx,sy) en framebuffer [0..SCREEN_W-1][0..SCREEN_H-1]:
//     log_x = sy           (columna horizontal del log)
//     log_y = ARB_BMP_H - 1 - sx + ARB_VIS_Y0 - 0  ... ajuste de recorte
// La relacion exacta con recorte y=[11,242]:
//   screen tiene SCREEN_W=232 columnas → corresponden a log_y=[11,242]
//   sx=0 → log_y=ARB_VIS_Y1=242; sx=231 → log_y=ARB_VIS_Y0=11
// ---------------------------------------------------------------------------

void arabian_render(Arabian* a) {
    // Render sin rotacion: mostramos el area visible del bitmap (x=0..255, y=11..242)
    // en una pantalla 256x232 (ARB_SCREEN_W x ARB_SCREEN_H)
    for (int sy = 0; sy < ARB_SCREEN_H; sy++) {      // 0..231
        int log_y = ARB_VIS_Y0 + sy;                // 11..242
        for (int sx = 0; sx < ARB_SCREEN_W; sx++) { // 0..255
            int log_x = sx;

            uint32_t color;
            // Limites por seguridad (deberian cumplirse siempre)
            if ((unsigned)log_x >= ARB_BMP_W || (unsigned)log_y >= ARB_BMP_H) {
                color = 0xFF000000u;
            } else {
                int idx1 = a->bmp1[log_y * ARB_BMP_W + log_x];
                int idx2 = a->bmp2[log_y * ARB_BMP_W + log_x];
                // Robustez: por si algun codigo antiguo guardo bmp2 como 16+idx, normalizamos a 0..15
                if (idx1 >= 16) idx1 &= 0x0F;
                if (idx2 >= 16) idx2 -= 16;

                // Composicion: bmp2 encima de bmp1; transparente = ARB_TRANSPARENT
                if (idx2 != ARB_TRANSPARENT) {
                    color = a->palette[16 + idx2];
                } else if (idx1 != ARB_TRANSPARENT) {
                    color = a->palette[idx1];
                } else {
                    color = 0xFF000000u;
                }
            }
            a->framebuffer[sy * ARB_SCREEN_W + sx] = color;
        }
    }

    SDL_UpdateTexture(a->texture, NULL, a->framebuffer,
                      ARB_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(a->renderer);
    SDL_Rect dst = { 0, 0, ARB_SCREEN_W * ARB_SCALE, ARB_SCREEN_H * ARB_SCALE };
    SDL_RenderCopy(a->renderer, a->texture, NULL, &dst);
    SDL_RenderPresent(a->renderer);
}


// ---------------------------------------------------------------------------
// arabian_handle_key
// Input active-HIGH (al contrario que Phoenix y Minivader)
// ---------------------------------------------------------------------------

void arabian_handle_key(Arabian* a, SDL_Scancode sc, bool pressed) {
    switch (sc) {
    // Joystick P1 → in[1] bits 0-3
    case SDL_SCANCODE_RIGHT: if (pressed) a->in[1] |= 0x01; else a->in[1] &= ~0x01; break;
    case SDL_SCANCODE_LEFT:  if (pressed) a->in[1] |= 0x02; else a->in[1] &= ~0x02; break;
    case SDL_SCANCODE_UP:    if (pressed) a->in[1] |= 0x04; else a->in[1] &= ~0x04; break;
    case SDL_SCANCODE_DOWN:  if (pressed) a->in[1] |= 0x08; else a->in[1] &= ~0x08; break;
    // Fire P1 → in[2] bit 0
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_LCTRL:
        if (pressed) a->in[2] |= 0x01; else a->in[2] &= ~0x01; break;
    // Start / Coin → in[0]
    case SDL_SCANCODE_1:
        if (pressed) a->in[0] |= 0x02; else a->in[0] &= ~0x02; break;  // START1
    case SDL_SCANCODE_2:
        if (pressed) a->in[0] |= 0x04; else a->in[0] &= ~0x04; break;  // START2
    case SDL_SCANCODE_5:
        if (pressed) a->dsw0 |= 0x01; else a->dsw0 &= ~0x01; break;    // COIN1
    case SDL_SCANCODE_6:
        if (pressed) a->dsw0 |= 0x02; else a->dsw0 &= ~0x02; break;    // COIN2
    // Servicio
    case SDL_SCANCODE_F2:
        if (pressed) a->dsw0 |= 0x04; else a->dsw0 &= ~0x04; break;    // TEST
    // Portar B: forzar lectura de switches (normalmente lo hace el juego via AY)
    case SDL_SCANCODE_F9:
        if (pressed) a->ay_portB |= 0x10; else a->ay_portB &= ~0x10; break;
    case SDL_SCANCODE_ESCAPE:
        if (pressed) { a->quit = true; } break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void print_usage(const char* exe) {
    printf("Uso: %s [opciones]\n\n", exe);
    printf("Emulador de Arabian (Sun Electronics, 1983)\n\n");
    printf("Opciones:\n");
    printf("  --rom <fichero>         ROM de programa completa (32KB concatenada)\n");
    printf("  --ic1 <f> [--ic2 ..4]  ROMs de programa individuales (8KB cada una)\n");
    printf("  --gfx <fichero>         GFX ROM completa (32KB)\n");
    printf("  --ic84 <f> [--ic85..7] GFX ROMs individuales (8KB cada una)\n\n");
    printf("Ejemplo:\n");
    printf("  %s --ic1 ic1rev2.87 --ic2 ic2rev2.88 --ic3 ic3rev2.89 --ic4 ic4rev2.90 \\\n", exe);
    printf("      --ic84 ic84.91 --ic85 ic85.92 --ic86 ic86.93 --ic87 ic87.94\n\n");
    printf("Controles:\n");
    printf("  Flechas        Joystick\n");
    printf("  Espacio/Ctrl   Disparar\n");
    printf("  1 / 2          Start 1 / Start 2\n");
    printf("  5 / 6          Coin 1 / Coin 2\n");
    printf("  F2             Service / Test\n");
    printf("  Escape         Salir\n");
}

int main(int argc, char* argv[]) {
    static Arabian a;
    arabian_init(&a);
    arabian_build_palette(&a);
    // Inicialmente, los bitmaps estan vacios: llenamos con el color transparente (8)
    memset(a.bmp1, ARB_TRANSPARENT, sizeof(a.bmp1));
    memset(a.bmp2, ARB_TRANSPARENT, sizeof(a.bmp2));

    const char* rom_full = NULL;
    const char* gfx_full = NULL;
    const char* rom_ic[4]  = {NULL,NULL,NULL,NULL};
    const char* gfx_ic[4]  = {NULL,NULL,NULL,NULL};

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--rom") == 0  && i+1<argc) rom_full   = argv[++i];
        else if (strcmp(argv[i], "--gfx") == 0  && i+1<argc) gfx_full   = argv[++i];
        else if (strcmp(argv[i], "--ic1") == 0  && i+1<argc) rom_ic[0]  = argv[++i];
        else if (strcmp(argv[i], "--ic2") == 0  && i+1<argc) rom_ic[1]  = argv[++i];
        else if (strcmp(argv[i], "--ic3") == 0  && i+1<argc) rom_ic[2]  = argv[++i];
        else if (strcmp(argv[i], "--ic4") == 0  && i+1<argc) rom_ic[3]  = argv[++i];
        else if (strcmp(argv[i], "--ic84") == 0 && i+1<argc) gfx_ic[0]  = argv[++i];
        else if (strcmp(argv[i], "--ic85") == 0 && i+1<argc) gfx_ic[1]  = argv[++i];
        else if (strcmp(argv[i], "--ic86") == 0 && i+1<argc) gfx_ic[2]  = argv[++i];
        else if (strcmp(argv[i], "--ic87") == 0 && i+1<argc) gfx_ic[3]  = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]); arabian_destroy(&a); return 0;
        }
    }

    if (argc < 2) { print_usage(argv[0]); arabian_destroy(&a); return 0; }

    // Cargar ROM de programa
    if (rom_full) {
        arabian_load_rom(&a, rom_full);
    } else {
        bool any = false;
        for (int i = 0; i < 4; i++)
            if (rom_ic[i] && arabian_load_rom_chunk(&a, rom_ic[i], i*0x2000, 0x2000)==0) any=true;
        a.have_rom = any;
    }

    // Cargar GFX ROM
    bool have_gfx = false;
    if (gfx_full) {
        have_gfx = (arabian_load_gfx(&a, gfx_full, 0, 0) == 0);
    } else {
        for (int i = 0; i < 4; i++)
            if (gfx_ic[i] && arabian_load_gfx(&a, gfx_ic[i], i*0x2000, 0x2000)==0) have_gfx=true;
    }

    if (have_gfx) {
        a.have_gfx = true;
        arabian_decode_gfx(&a);
    } else {
        fprintf(stderr, "Aviso: Sin GFX ROMs. No habra graficos del blitter.\n");
    }

    if (!a.have_rom)
        fprintf(stderr, "Aviso: ROM de programa no cargada.\n");

    // portB bit4 = 1 para que el juego lea los switches desde el principio
    a.ay_portB = 0x10;

    const uint32_t FRAME_MS = 1000 / ARB_FPS;
    printf("Arabian - iniciando emulacion. ESC para salir.\n");

    while (!a.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) a.quit = true;
            else if (e.type == SDL_KEYDOWN) arabian_handle_key(&a, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)   arabian_handle_key(&a, e.key.keysym.scancode, false);
        }

        arabian_run_frame(&a);

        if (a.turbo_mode) {
            if ((a.frame_counter & 7) == 0) arabian_render(&a);
        } else {
            arabian_render(&a);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS) SDL_Delay(FRAME_MS - elapsed);
        }
    }

    arabian_destroy(&a);
    return 0;
}
