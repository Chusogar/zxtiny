/*
 * tehkanwc.c  -  Emulador de Tehkan World Cup (Tehkan, 1985)
 *
 * Estructura y estilo basados en zx.c / minivadr.c / phoenix.c /
 * arabian.c / commando.c del proyecto zxtiny (chusogar/zxtiny).
 * Driver original: MAME 0.37b7 drivers/tehkanwc.c + vidhrdw/tehkanwc.c
 *                  (Ernesto Corvi, Roberto Juan Fresca)
 */

#include "tehkanwc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

static int load_file(uint8_t* dst, int max_size, const char* path,
                     int offset, int size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    int to_read = (size > 0) ? size : (int)fsz;
    if (offset + to_read > max_size) {
        fprintf(stderr, "[ROM] '%s': desbordamiento (off=%d sz=%d max=%d)\n",
                path, offset, to_read, max_size);
        fclose(f); return -1;
    }
    int n = (int)fread(dst + offset, 1, (size_t)to_read, f);
    fclose(f);
    if (n != to_read) {
        fprintf(stderr, "[ROM] '%s': leidos %d de %d\n", path, n, to_read);
        return -1;
    }
    printf("[ROM] '%s' -> off=0x%05X (%d bytes)\n", path, offset, n);
    return 0;
}

// ---------------------------------------------------------------------------
// Decodificación de GFX
//
// Los tres layouts comparten el mismo esquema de "nibbles intercambiados":
//   pixels[] = { 1*4, 0*4, 3*4, 2*4, 5*4, 4*4, 7*4, 6*4 }
//   → los pixels se extraen en PARES de nibbles, con los nibbles swapped.
//   Cada byte de ROM contiene 2 pixels (nibble alto y nibble bajo),
//   pero el orden dentro de cada par de pixels (0,1) → (1,0) está invertido.
//
// Para un byte de ROM b:
//   nibble_hi = (b >> 4) & 0x0F   → pixel en posición impar del par
//   nibble_lo =  b       & 0x0F   → pixel en posición par del par
//
// Los "bits per pixel" son { 0,1,2,3 } = los 4 bits del nibble directamente.
// ---------------------------------------------------------------------------

// Extrae el pixel p (0-based) de una secuencia de nibbles en la ROM.
// La secuencia de pixels[] del charlayout/spritelayout/tilelayout define
// el bit_offset dentro del tile. Cada "nibble" ocupa 4 bits.
// pixels[c] = bit_offset; byte = bit_offset/8; nibble = (bit_offset/4)%2
// Para pixels[] = {4,0,12,8,20,16,28,24,...}:
//   pixel 0 → bit_offset=4  → byte=0, nibble_hi
//   pixel 1 → bit_offset=0  → byte=0, nibble_lo
//   pixel 2 → bit_offset=12 → byte=1, nibble_hi
//   pixel 3 → bit_offset=8  → byte=1, nibble_lo
//   etc.
// Simplificado: para col c en [0,7]:
//   pair = c / 2         → par de pixels (0,1,2,3)
//   is_hi = (c % 2) == 0 → pixel par=nibble_hi, impar=nibble_lo
//   byte_in_row = pair   → byte dentro de la fila (de 4 bytes)
//   pix = is_hi ? (rom[byte_in_row] >> 4) & 0xF : rom[byte_in_row] & 0xF

// Chars: 8x8, 4bpp, stride=32 bytes, 4 bytes por fila
// rows[r] = r*32 → fila r empieza en byte r*4 dentro del tile
#if 1
static void decode_chars(TehkanWC* t) {
    for (int tile = 0; tile < TWC_CHARS_NUM; tile++) {
        for (int row = 0; row < TWC_CHAR_H; row++) {
            int row_base = tile * 32 + row * 4;
            for (int col = 0; col < TWC_CHAR_W; col++) {
                int pair    = col / 2;
                bool is_hi  = (col & 1) != 0;  // pixel par → nibble alto REJILLA CORRECTA
				//bool is_hi  = (col & 1) == 0;  // REJILLA INCORRECTA
                int  off    = row_base + pair;
                if (off >= TWC_GFX1_SIZE) continue;
                uint8_t b   = t->gfx1[off];
                t->char_pix[tile][row][col] = is_hi ? (b >> 4) & 0xF : b & 0xF;
            }
        }
    }
}
#endif




// Sprites: 16x16, 4bpp, stride=128 bytes
// rows[r] = r*32 para r<8, luego 16*32 + (r-8)*32 para r>=8
//   (rows: { 0*32..7*32, 16*32..23*32 })
// pixels cols 0-7: como chars (pair c/2, byte en row_base+pair)
// pixels cols 8-15: offset extra de 8*32 = 256 bits = 32 bytes
//   → byte_offset_extra = 32 (= 8 columnas * 4 bits/col en nibbles = 32 bytes)
static void decode_sprites(TehkanWC* t) {
    for (int s = 0; s < TWC_SPRITES_NUM; s++) {
        for (int row = 0; row < TWC_SPR_H; row++) {
            // row offset: 0..7 → row*32; 8..15 → 16*32 + (row-8)*32
            int row_byte = (row < 8) ? row * 4 : (16 + (row - 8)) * 4;
            int tile_base = s * 128;
            for (int col = 0; col < TWC_SPR_W; col++) {
                // cols 0-7: bytes 0-3 de la fila; cols 8-15: +32 bytes (=8 pares más)
                int col_group = col / 8;  // 0=izquierda, 1=derecha
                int col_in_group = col & 7;
                int pair = col_in_group / 2;
                bool is_hi = (col_in_group & 1) == 0;
                int off = tile_base + row_byte + col_group * 32 + pair;
                if (off >= TWC_GFX2_SIZE) continue;
                uint8_t b = t->gfx2[off];
                t->spr_pix[s][row][col] = is_hi ? (b >> 4) & 0xF : b & 0xF;
            }
        }
    }
}

#if 0
static void decode_tiles(TehkanWC* t) {
    // BG Tiles: 16x8, 4bpp, 64 bytes por tile.
    // Filas lineales (8 bytes por fila) + orden nibble "swapped"
    // (col par usa nibble alto, col impar usa nibble bajo) -> xoffset {4,0,12,8,...}
    for (int tile = 0; tile < TWC_BGTILES_NUM; tile++) {
        int tile_base = tile * 64;
        for (int row = 0; row < TWC_TILE_H; row++) {
            int row_base = tile_base + row * 8;
            for (int col = 0; col < TWC_TILE_W; col++) {
                int byte_in_row = col >> 1;          // 0..7
                bool is_hi = (col & 1) == 0;         // col par -> nibble alto (swapped)
                int off = row_base + byte_in_row;
                if (off >= TWC_GFX3_SIZE) continue;
                uint8_t b = t->gfx3[off];
                t->tile_pix[tile][row][col] = is_hi ? ((b >> 4) & 0xF) : (b & 0xF);
            }
        }
    }
}
#endif

#if 1
// BG Tiles: 16x8, 4bpp, stride=64 bytes
// rows[r] = r*32 para r<8 (8 filas de 4 bytes cada una)
// pixels cols 0-7: bytes 0-3; cols 8-15: +32 bytes (=32*8 bits)
static void decode_tiles(TehkanWC* t) {
    for (int tile = 0; tile < TWC_BGTILES_NUM; tile++) {
        for (int row = 0; row < TWC_TILE_H; row++) {
            int row_byte  = row * 4;
            int tile_base = tile * 64;
            for (int col = 0; col < TWC_TILE_W; col++) {
                int col_group   = col / 8;
                int col_in_group = col & 7;
                int pair   = col_in_group / 2;
                bool is_hi = (col_in_group & 1) == 0;
                int off    = tile_base + row_byte + col_group * 32 + pair;
                if (off >= TWC_GFX3_SIZE) continue;
                uint8_t b = t->gfx3[off];
                t->tile_pix[tile][row][col] = is_hi ? (b >> 4) & 0xF : b & 0xF;
            }
        }
    }
}
#endif

void tehkanwc_decode_gfx(TehkanWC* t) {
    if (t->char_pix) decode_chars(t);
    if (t->spr_pix)  decode_sprites(t);
    if (t->tile_pix) decode_tiles(t);
}

// ---------------------------------------------------------------------------
// Paleta: formato xxxxBBBBGGGGRRRR con byte-swap
// paletteram[offs]=byte_lo=GGGGRRRRR, paletteram[offs+1]=byte_hi=xxxxBBBB
// word = (paletteram[offs+1]<<8) | paletteram[offs]
// R = (word & 0x000F) * 17
// G = ((word >> 4) & 0x000F) * 17
// B = ((word >> 8) & 0x000F) * 17
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Paleta: formato xxxxBBBBGGGGRRRR con byte-swap
// RAM: palram[off] = byte_low, palram[off+1] = byte_high (según bus)
// En esta placa el word efectivo se obtiene con bytes invertidos.
// ---------------------------------------------------------------------------
static void rebuild_palette(TehkanWC* t)
{
    // palram size = 0x600 bytes => 0x300 words => 768 colores
    const int ncolors = TWC_PALRAM_SIZE / 2;
    for (int i = 0; i < ncolors && i < TWC_TOTAL_COLORS; i++) {
        const int off = i * 2;

        // BYTE SWAP REAL: el orden en RAM no coincide con el word lógico
        uint16_t word = (uint16_t)(t->palram[off + 1] | (t->palram[off] << 8));

        uint8_t r = (uint8_t)(( word        & 0x000F) * 17); // 0..15 -> 0..255
        uint8_t g = (uint8_t)(((word >> 4)  & 0x000F) * 17);
        uint8_t b = (uint8_t)(((word >> 8)  & 0x000F) * 17);

        t->palette[i] = 0xFF000000u |
                        ((uint32_t)r << 16) |
                        ((uint32_t)g <<  8) |
                        (uint32_t)b;
    }
}

// ---------------------------------------------------------------------------
// VRAM compartida: lectura y escritura con dirty flags
// ---------------------------------------------------------------------------

static inline uint8_t vram_read(TehkanWC* t, uint16_t addr) {
    if (addr >= TWC_FGVRAM_START && addr < TWC_FGVRAM_START + TWC_FGVRAM_SIZE)
        return t->fgvram[addr - TWC_FGVRAM_START];
    if (addr >= TWC_FGCRAM_START && addr < TWC_FGCRAM_START + TWC_FGVRAM_SIZE)
        return t->fgcram[addr - TWC_FGCRAM_START];
    if (addr >= TWC_PALRAM_START && addr < TWC_PALRAM_START + TWC_PALRAM_SIZE)
        return t->palram[addr - TWC_PALRAM_START];
    if (addr >= TWC_BGVRAM_START && addr < TWC_BGVRAM_START + TWC_BGVRAM_SIZE)
        return t->bgvram[addr - TWC_BGVRAM_START];
    if (addr >= TWC_SPRITERAM_START && addr < TWC_SPRITERAM_START + TWC_SPRITERAM_SIZE)
        return t->spriteram[addr - TWC_SPRITERAM_START];
    if (addr == TWC_SCROLL_X_LO) return t->scroll_x[0];
    if (addr == TWC_SCROLL_X_HI) return t->scroll_x[1];
    if (addr == TWC_SCROLL_Y)    return t->scroll_y;
    return 0xFF;
}

static inline void vram_write(TehkanWC* t, uint16_t addr, uint8_t val) {
    if (addr >= TWC_FGVRAM_START && addr < TWC_FGVRAM_START + TWC_FGVRAM_SIZE) {
        t->fgvram[addr - TWC_FGVRAM_START] = val; return;
    }
    if (addr >= TWC_FGCRAM_START && addr < TWC_FGCRAM_START + TWC_FGVRAM_SIZE) {
        t->fgcram[addr - TWC_FGCRAM_START] = val; return;
    }
    if (addr >= TWC_PALRAM_START && addr < TWC_PALRAM_START + TWC_PALRAM_SIZE) {
        t->palram[addr - TWC_PALRAM_START] = val;
        rebuild_palette(t);
        return;
    }
    if (addr >= TWC_BGVRAM_START && addr < TWC_BGVRAM_START + TWC_BGVRAM_SIZE) {
        int off = addr - TWC_BGVRAM_START;
        t->bgvram[off] = val;
        t->bg_dirty[off >> 1] = true;
        return;
    }
    if (addr >= TWC_SPRITERAM_START && addr < TWC_SPRITERAM_START + TWC_SPRITERAM_SIZE) {
        t->spriteram[addr - TWC_SPRITERAM_START] = val; return;
    }
    if (addr == TWC_SCROLL_X_LO) { t->scroll_x[0] = val; return; }
    if (addr == TWC_SCROLL_X_HI) { t->scroll_x[1] = val; return; }
    if (addr == TWC_SCROLL_Y)    { t->scroll_y = val;     return; }
}

// ---------------------------------------------------------------------------
// Callbacks CPU main
// ---------------------------------------------------------------------------

static uint8_t main_read(void* ud, uint16_t addr) {
    TehkanWC* t = (TehkanWC*)ud;
    if (addr <= 0xBFFF)  return t->mainrom[addr];
    if (addr >= 0xC000 && addr <= 0xC7FF) return t->mainram[addr - 0xC000];
    if (addr >= 0xC800 && addr <= 0xCFFF) return t->shared[addr - 0xC800];
    if ((addr >= TWC_FGVRAM_START  && addr <= 0xEBFF) ||
        (addr >= TWC_SCROLL_X_LO   && addr <= TWC_SCROLL_Y))
        return vram_read(t, addr);
    // Input
    if (addr == 0xF800) return (uint8_t)t->track0[0];
    if (addr == 0xF801) return (uint8_t)t->track0[1];
    if (addr == 0xF802) return t->coins;
    if (addr == 0xF803) return t->btn0;
    if (addr == 0xF810) return (uint8_t)t->track1[0];
    if (addr == 0xF811) return (uint8_t)t->track1[1];
    if (addr == 0xF813) return t->btn1;
    if (addr == 0xF820) return t->sound_answer;
    if (addr == 0xF840) return t->dsw[0];
    if (addr == 0xF850) return t->dsw[1];
    if (addr == 0xF860) return 0xFF;  // watchdog
    if (addr == 0xF870) return t->dsw[2];
    return 0xFF;
}

static void main_write(void* ud, uint16_t addr, uint8_t val) {
    TehkanWC* t = (TehkanWC*)ud;
    if (addr <= 0xBFFF) return;
    if (addr >= 0xC000 && addr <= 0xC7FF) { t->mainram[addr - 0xC000] = val; return; }
    if (addr >= 0xC800 && addr <= 0xCFFF) { t->shared[addr - 0xC800]  = val; return; }
    if (addr >= TWC_FGVRAM_START && addr <= 0xEC02) { vram_write(t, addr, val); return; }
    // Track reset
    if (addr == 0xF800) { t->track0[0] = (int8_t)val; return; }
    if (addr == 0xF801) { t->track0[1] = (int8_t)val; return; }
    if (addr == 0xF810) { t->track1[0] = (int8_t)val; return; }
    if (addr == 0xF811) { t->track1[1] = (int8_t)val; return; }
    // Sound command → provoca NMI en la CPU de sonido
    if (addr == 0xF820) {
        t->soundlatch = val;
        t->snd_nmi_pending = true;
        return;
    }
    // Sub CPU halt (bit0=1 → clear reset, bit0=0 → assert reset)
    if (addr == 0xF840) {
        t->sub_halted = (val == 0);
        return;
    }
}

static uint8_t main_port_in(z80* z, uint16_t p)  { (void)z;(void)p; return 0xFF; }
static void    main_port_out(z80* z, uint16_t p, uint8_t v) { (void)z;(void)p;(void)v; }

// ---------------------------------------------------------------------------
// Callbacks CPU sub (misma VRAM compartida, ROM propia)
// ---------------------------------------------------------------------------

static uint8_t sub_read(void* ud, uint16_t addr) {
    TehkanWC* t = (TehkanWC*)ud;
    if (addr <= 0x7FFF)  return t->subrom[addr];
    if (addr >= 0x8000 && addr <= 0xC7FF) return t->subram[addr - 0x8000];
    if (addr >= 0xC800 && addr <= 0xCFFF) return t->shared[addr - 0xC800];
    if ((addr >= TWC_FGVRAM_START && addr <= 0xEBFF) ||
        (addr >= TWC_SCROLL_X_LO  && addr <= TWC_SCROLL_Y))
        return vram_read(t, addr);
    if (addr == 0xF860) return 0xFF;  // watchdog
    return 0xFF;
}

static void sub_write(void* ud, uint16_t addr, uint8_t val) {
    TehkanWC* t = (TehkanWC*)ud;
    if (addr <= 0x7FFF) return;
    if (addr >= 0xC000 && addr <= 0xC7FF) { t->subram[addr - 0x8000] = val; return; }
    if (addr >= 0xC800 && addr <= 0xCFFF) { t->shared[addr - 0xC800]  = val; return; }
    if (addr >= TWC_FGVRAM_START && addr <= 0xEC02) { vram_write(t, addr, val); return; }
}

static uint8_t sub_port_in(z80* z, uint16_t p)  { (void)z;(void)p; return 0xFF; }
static void    sub_port_out(z80* z, uint16_t p, uint8_t v) { (void)z;(void)p;(void)v; }

// ---------------------------------------------------------------------------
// Callbacks CPU sound
// ---------------------------------------------------------------------------

static uint8_t snd_read(void* ud, uint16_t addr) {
    TehkanWC* t = (TehkanWC*)ud;
    if (addr <= 0x3FFF)  return t->sndrom[addr];
    if (addr >= 0x4000 && addr <= 0x47FF) return t->sndram[addr - 0x4000];
    if (addr == 0xC000)  return t->soundlatch;
    return 0xFF;
}

static void snd_write(void* ud, uint16_t addr, uint8_t val) {
    TehkanWC* t = (TehkanWC*)ud;
    if (addr >= 0x4000 && addr <= 0x47FF) { t->sndram[addr - 0x4000] = val; return; }
    if (addr == 0xC000) { t->sound_answer = val; return; }
    // 0x8001: MSM reset, 0x8002-0x8003: NOP → ignorados
}

static uint8_t snd_port_in(z80* z, uint16_t p)  { (void)z;(void)p; return 0xFF; }
static void    snd_port_out(z80* z, uint16_t p, uint8_t v) { (void)z;(void)p;(void)v; }

// ---------------------------------------------------------------------------
// tehkanwc_init
// ---------------------------------------------------------------------------

void tehkanwc_init(TehkanWC* t) {
    memset(t, 0, sizeof(*t));

    // Heap para tiles
    t->char_pix = calloc(TWC_CHARS_NUM,   sizeof(*t->char_pix));
    t->spr_pix  = calloc(TWC_SPRITES_NUM, sizeof(*t->spr_pix));
    t->tile_pix = calloc(TWC_BGTILES_NUM, sizeof(*t->tile_pix));

    // Input active-low
    t->btn0  = 0xFF;
    t->btn1  = 0xFF;
    t->coins = 0xFF;
    t->dsw[0] = t->dsw[1] = t->dsw[2] = 0xFF;

    // Todo BG dirty
    memset(t->bg_dirty, 1, sizeof(t->bg_dirty));

    // Paleta inicial negro
    for (int i = 0; i < TWC_TOTAL_COLORS; i++)
        t->palette[i] = 0xFF000000u;

    // BG bitmap negro
    for (int i = 0; i < TWC_BGBMP_W * TWC_BGBMP_H; i++)
        t->bgbmp[i] = 0xFF000000u;

    // CPUs
    z80_init(&t->cpu_main);
    t->cpu_main.userdata   = t;
    t->cpu_main.read_byte  = main_read;
    t->cpu_main.write_byte = main_write;
    t->cpu_main.port_in    = main_port_in;
    t->cpu_main.port_out   = main_port_out;

    z80_init(&t->cpu_sub);
    t->cpu_sub.userdata    = t;
    t->cpu_sub.read_byte   = sub_read;
    t->cpu_sub.write_byte  = sub_write;
    t->cpu_sub.port_in     = sub_port_in;
    t->cpu_sub.port_out    = sub_port_out;

    z80_init(&t->cpu_snd);
    t->cpu_snd.userdata    = t;
    t->cpu_snd.read_byte   = snd_read;
    t->cpu_snd.write_byte  = snd_write;
    t->cpu_snd.port_in     = snd_port_in;
    t->cpu_snd.port_out    = snd_port_out;

    // SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    t->window = SDL_CreateWindow("Tehkan World Cup (Tehkan, 1985)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        TWC_SCREEN_W * TWC_SCALE, TWC_SCREEN_H * TWC_SCALE,
        SDL_WINDOW_SHOWN);
    t->renderer = SDL_CreateRenderer(t->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    t->texture = SDL_CreateTexture(t->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, TWC_SCREEN_W, TWC_SCREEN_H);
    SDL_RenderSetLogicalSize(t->renderer,
        TWC_SCREEN_W * TWC_SCALE, TWC_SCREEN_H * TWC_SCALE);
}

void tehkanwc_destroy(TehkanWC* t) {
    free(t->char_pix); free(t->spr_pix); free(t->tile_pix);
    if (t->texture)  SDL_DestroyTexture(t->texture);
    if (t->renderer) SDL_DestroyRenderer(t->renderer);
    if (t->window)   SDL_DestroyWindow(t->window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Carga de ROMs
// ---------------------------------------------------------------------------

int tehkanwc_load_mainrom(TehkanWC* t, const char* path, int offset, int size) {
    int r = load_file(t->mainrom, TWC_MAINROM_SIZE, path, offset, size);
    if (r == 0) t->have_mainrom = true;
    return r;
}
int tehkanwc_load_subrom(TehkanWC* t, const char* path) {
    int r = load_file(t->subrom, TWC_SUBROM_SIZE, path, 0, 0);
    if (r == 0) t->have_subrom = true;
    return r;
}
int tehkanwc_load_sndrom(TehkanWC* t, const char* path) {
    int r = load_file(t->sndrom, TWC_SNDROM_SIZE, path, 0, 0);
    if (r == 0) t->have_sndrom = true;
    return r;
}
int tehkanwc_load_gfx1(TehkanWC* t, const char* path) {
    int r = load_file(t->gfx1, TWC_GFX1_SIZE, path, 0, 0);
    if (r == 0) t->have_gfx1 = true;
    return r;
}
int tehkanwc_load_gfx2(TehkanWC* t, const char* path, int offset, int size) {
    int r = load_file(t->gfx2, TWC_GFX2_SIZE, path, offset, size);
    if (r == 0) t->have_gfx2 = true;
    return r;
}
int tehkanwc_load_gfx3(TehkanWC* t, const char* path, int offset, int size) {
    int r = load_file(t->gfx3, TWC_GFX3_SIZE, path, offset, size);
    if (r == 0) t->have_gfx3 = true;
    return r;
}

// ---------------------------------------------------------------------------
// Renderizado
// ---------------------------------------------------------------------------

// Color de FG char (base 0, 16 colores por grupo)
static inline uint32_t fg_col(TehkanWC* t, int color, int pix) {
    return t->palette[(TWC_CHAR_COLOR_BASE + color * 16 + pix) % TWC_TOTAL_COLORS];
}
// Color de sprite (base 256, 16 colores por grupo)
static inline uint32_t spr_col(TehkanWC* t, int color, int pix) {
    return t->palette[(TWC_SPR_COLOR_BASE + color * 16 + pix) % TWC_TOTAL_COLORS];
}
// Color de BG tile (base 512, 16 colores por grupo)
static inline uint32_t bg_col(TehkanWC* t, int color, int pix) {
    return t->palette[(TWC_BG_COLOR_BASE + color * 16 + pix) % TWC_TOTAL_COLORS];
}

// Dibuja un tile BG 16x8 en el bgbmp (doble ancho, sin recorte)
static void draw_bg_tile(TehkanWC* t, int code, int color,
                          int flipx, int flipy, int sx, int sy) {
    if (!t->tile_pix) return;
    code %= TWC_BGTILES_NUM;
    for (int row = 0; row < TWC_TILE_H; row++) {
        int py = sy + (flipy ? TWC_TILE_H - 1 - row : row);
        if (py < 0 || py >= TWC_BGBMP_H) continue;
        for (int col = 0; col < TWC_TILE_W; col++) {
            int px = sx + (flipx ? TWC_TILE_W - 1 - col : col);
            // wrap horizontal en el BG bitmap de 512 de ancho
            px = ((px % TWC_BGBMP_W) + TWC_BGBMP_W) % TWC_BGBMP_W;
            t->bgbmp[py * TWC_BGBMP_W + px] =
                bg_col(t, color, t->tile_pix[code][row][col]);
        }
    }
}

// Renderiza el BG (tiles sucios)
// Layout BG VRAM: 2 bytes por tile, sx=(offs%64)*8, sy=(offs/64)*8
// El mapa tiene 64 columnas x 16 filas de tiles 16x8 = 512x128 pixels
static void render_bg(TehkanWC* t) {
    int ntiles = TWC_BGVRAM_SIZE / 2;
    for (int idx = 0; idx < ntiles; idx++) {
        if (!t->bg_dirty[idx]) continue;
        t->bg_dirty[idx] = false;
        int off  = idx * 2;
        uint8_t b0 = t->bgvram[off];
        uint8_t b1 = t->bgvram[off + 1];
        int code  = b0 + ((b1 & 0x30) << 4);  // 10 bits
        int color = b1 & 0x0F;
        int flipx = (b1 >> 6) & 1;
        int flipy = (b1 >> 7) & 1;
        int sx    = (off % 64) * 8;
        int sy    = (off / 64) * 8;
        draw_bg_tile(t, code, color, flipx, flipy, sx, sy);
    }
}

// Copia BG bitmap al framebuffer con scroll
static void blit_bg(TehkanWC* t) {
    int scrollx = (int)t->scroll_x[0] + ((int)t->scroll_x[1] << 8);
    int scrolly = (int)t->scroll_y;
    // El BG bitmap es de 512x256; la pantalla visible es 256x224 (y=[16,239])
    for (int sy = 0; sy < TWC_SCREEN_H; sy++) {
        int src_y = ((sy + TWC_VIS_Y0 + scrolly) % TWC_BGBMP_H + TWC_BGBMP_H) % TWC_BGBMP_H;
        for (int sx = 0; sx < TWC_SCREEN_W; sx++) {
            int src_x = ((sx + scrollx) % TWC_BGBMP_W + TWC_BGBMP_W) % TWC_BGBMP_W;
            t->framebuffer[sy * TWC_SCREEN_W + sx] =
                t->bgbmp[src_y * TWC_BGBMP_W + src_x];
        }
    }
}

// Dibuja FG char en framebuffer con clipping a area visible
static void draw_fg_char(TehkanWC* t, int code, int color,
                          int flipx, int flipy, int sx, int sy,
                          bool transparent) {
    if (!t->char_pix) return;
    code %= TWC_CHARS_NUM;
    for (int row = 0; row < TWC_CHAR_H; row++) {
        int py = sy + (flipy ? TWC_CHAR_H - 1 - row : row);
        // Recortar a area visible (y=[16,239] en coordenadas de tile map = [0,223] en framebuffer)
        if (py < TWC_VIS_Y0 || py > TWC_VIS_Y1) continue;
        int fy = py - TWC_VIS_Y0;
        for (int col = 0; col < TWC_CHAR_W; col++) {
            int px = sx + (flipx ? TWC_CHAR_W - 1 - col : col);
            if (px < 0 || px >= TWC_SCREEN_W) continue;
            uint8_t pix = t->char_pix[code][row][col];
            if (transparent && pix == 0) continue;
            t->framebuffer[fy * TWC_SCREEN_W + px] = fg_col(t, color, pix);
        }
    }
}

// Dibuja sprite en framebuffer con clipping
static void draw_sprite(TehkanWC* t, int code, int color,
                         int flipx, int flipy, int sx, int sy) {
    if (!t->spr_pix) return;
    code %= TWC_SPRITES_NUM;
    for (int row = 0; row < TWC_SPR_H; row++) {
        int py = sy + (flipy ? TWC_SPR_H - 1 - row : row);
        if (py < TWC_VIS_Y0 || py > TWC_VIS_Y1) continue;
        int fy = py - TWC_VIS_Y0;
        for (int col = 0; col < TWC_SPR_W; col++) {
            int px = sx + (flipx ? TWC_SPR_W - 1 - col : col);
            if (px < 0 || px >= TWC_SCREEN_W) continue;
            uint8_t pix = t->spr_pix[code][row][col];
            if (pix == 0) continue;  // transparente pen0
            t->framebuffer[fy * TWC_SCREEN_W + px] = spr_col(t, color, pix);
        }
    }
}

// Renderizado completo siguiendo el orden de tehkanwc_vh_screenrefresh:
//  1. BG (con scroll)
//  2. FG chars con priority bit5=1 (debajo de sprites)
//  3. Sprites
//  4. FG chars con priority bit5=0 (encima de sprites)
void tehkanwc_render(TehkanWC* t) {
    // 1. BG
    render_bg(t);
    blit_bg(t);

    // 2. FG chars que van DEBAJO de sprites (colorram bit5 = 1)
    for (int offs = 0; offs < (int)TWC_FGVRAM_SIZE; offs++) {
        uint8_t attr = t->fgcram[offs];
        if (!(attr & 0x20)) continue;  // solo bit5=1 aquí
        int code  = t->fgvram[offs] + ((attr & 0x10) << 4);
        int color = attr & 0x0F;
        int flipx = (attr >> 6) & 1;
        int flipy = (attr >> 7) & 1;
        int sx = (offs % 32) * 8;
        int sy = (offs / 32) * 8;
        draw_fg_char(t, code, color, flipx, flipy, sx, sy, true);
    }

    // 3. Sprites
    // MAME: drawgfx con sx = spriteram[2] + ((attr & 0x20)<<3) - 0x80
    for (int offs = 0; offs < (int)TWC_SPRITERAM_SIZE; offs += 4) {
        uint8_t code_lo = t->spriteram[offs];
        uint8_t attr    = t->spriteram[offs + 1];
        uint8_t sx_raw  = t->spriteram[offs + 2];
        uint8_t sy_raw  = t->spriteram[offs + 3];
        int code  = code_lo + ((attr & 0x08) << 5);  // bit3 → bit8 del code
        int color = attr & 0x07;
        int flipx = (attr >> 6) & 1;
        int flipy = (attr >> 7) & 1;
        int sx    = (int)sx_raw + (((attr & 0x20) << 3)) - 0x80;
        int sy    = (int)sy_raw;
        draw_sprite(t, code, color, flipx, flipy, sx, sy);
    }

    // 4. FG chars que van ENCIMA de sprites (colorram bit5 = 0)
    for (int offs = 0; offs < (int)TWC_FGVRAM_SIZE; offs++) {
        uint8_t attr = t->fgcram[offs];
        if (attr & 0x20) continue;  // solo bit5=0 aquí
        int code  = t->fgvram[offs] + ((attr & 0x10) << 4);
        int color = attr & 0x0F;
        int flipx = (attr >> 6) & 1;
        int flipy = (attr >> 7) & 1;
        int sx = (offs % 32) * 8;
        int sy = (offs / 32) * 8;
        draw_fg_char(t, code, color, flipx, flipy, sx, sy, true);
    }

    // Presentar
    SDL_UpdateTexture(t->texture, NULL, t->framebuffer,
                      TWC_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(t->renderer);
    SDL_Rect dst = { 0, 0, TWC_SCREEN_W * TWC_SCALE, TWC_SCREEN_H * TWC_SCALE };
    SDL_RenderCopy(t->renderer, t->texture, NULL, &dst);
    SDL_RenderPresent(t->renderer);
}

// ---------------------------------------------------------------------------
// tehkanwc_run_frame
// MAME: 10 slices por frame para mantener sincronizacion entre CPUs.
// Las tres CPUs reciben IRQ cada VBLANK (interrupt, 1).
// La CPU de sonido recibe adicionalmente NMI cuando se escribe soundlatch.
// ---------------------------------------------------------------------------

void tehkanwc_run_frame(TehkanWC* t) {
    int cycles_per_slice  = TWC_CYCLES_PER_FRAME / TWC_SLICES;

    for (int slice = 0; slice < TWC_SLICES; slice++) {
        z80_step_n(&t->cpu_main, cycles_per_slice);
        if (!t->sub_halted)
            z80_step_n(&t->cpu_sub, cycles_per_slice);
        z80_step_n(&t->cpu_snd, cycles_per_slice);

        // NMI al sonido si hay soundlatch pendiente
        if (t->snd_nmi_pending) {
            t->snd_nmi_pending = false;
            z80_pulse_nmi(&t->cpu_snd);
        }
    }

    // IRQ VBLANK a las tres CPUs (interrupt, 1)
    z80_pulse_irq(&t->cpu_main, 0xFF);
    if (!t->sub_halted)
        z80_pulse_irq(&t->cpu_sub, 0xFF);
    z80_pulse_irq(&t->cpu_snd, 0xFF);

    t->frame_counter++;
}

// ---------------------------------------------------------------------------
// tehkanwc_handle_key
// El hardware usa trackballs; emulamos el movimiento con las flechas.
// Los deltas se acumulan en track0/track1 y se resetean cuando el juego
// los lee (track_0_reset_w / track_1_reset_w).
// ---------------------------------------------------------------------------

void tehkanwc_handle_key(TehkanWC* t, SDL_Scancode sc, bool pressed) {
    const int DELTA = 8;  // velocidad del trackball simulado

    switch (sc) {
    // P1 trackball → flechas
    case SDL_SCANCODE_RIGHT: if (pressed) t->track0[0] += DELTA; break;
    case SDL_SCANCODE_LEFT:  if (pressed) t->track0[0] -= DELTA; break;
    case SDL_SCANCODE_DOWN:  if (pressed) t->track0[1] += DELTA; break;
    case SDL_SCANCODE_UP:    if (pressed) t->track0[1] -= DELTA; break;
    // P2 trackball → WASD
    case SDL_SCANCODE_D: if (pressed) t->track1[0] += DELTA; break;
    case SDL_SCANCODE_A: if (pressed) t->track1[0] -= DELTA; break;
    case SDL_SCANCODE_S: if (pressed) t->track1[1] += DELTA; break;
    case SDL_SCANCODE_W: if (pressed) t->track1[1] -= DELTA; break;
    // Botones (active-low en bit5)
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_SPACE:
        if (pressed) t->btn0 &= ~0x20; else t->btn0 |= 0x20; break;
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_RETURN:
        if (pressed) t->btn1 &= ~0x20; else t->btn1 |= 0x20; break;
    // Coins / Start (active-low)
    case SDL_SCANCODE_5: if (pressed) t->coins &= ~0x01; else t->coins |= 0x01; break;
    case SDL_SCANCODE_6: if (pressed) t->coins &= ~0x02; else t->coins |= 0x02; break;
    case SDL_SCANCODE_1: if (pressed) t->coins &= ~0x04; else t->coins |= 0x04; break;
    case SDL_SCANCODE_2: if (pressed) t->coins &= ~0x08; else t->coins |= 0x08; break;
    case SDL_SCANCODE_ESCAPE:
        if (pressed) { t->quit = true; } break;
	// Turbo
	case SDL_SCANCODE_F2:
		if (pressed) {
			t->turbo_mode = !t->turbo_mode;
			//if (!t->turbo_mode && t->audio_dev > 0) SDL_ClearQueuedAudio(t->audio_dev);
			printf("[EMU] Velocidad %s\n", t->turbo_mode ? "MAXIMA" : "normal");
		}
		return;
    case SDL_SCANCODE_F5:
        if (pressed) memset(t->bg_dirty, 1, sizeof(t->bg_dirty));
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void print_usage(const char* exe) {
    printf("Uso: %s [opciones]\n\n", exe);
    printf("Emulador de Tehkan World Cup (Tehkan, 1985)\n\n");
    printf("ROMs de programa:\n");
    printf("  --twc1 <f>   twc-1.bin   0x0000-0x3FFF (16KB, CPU main)\n");
    printf("  --twc2 <f>   twc-2.bin   0x4000-0x7FFF (16KB, CPU main)\n");
    printf("  --twc3 <f>   twc-3.bin   0x8000-0xBFFF (16KB, CPU main)\n");
    printf("  --twc4 <f>   twc-4.bin   (32KB, CPU sub)\n");
    printf("  --twc6 <f>   twc-6.bin   (16KB, CPU sound)\n");
    printf("GFX:\n");
    printf("  --twc12 <f>  twc-12.bin  (16KB, FG chars)\n");
    printf("  --twc8  <f>  twc-8.bin   (32KB, sprites parte 0)\n");
    printf("  --twc7  <f>  twc-7.bin   (32KB, sprites parte 1)\n");
    printf("  --twc11 <f>  twc-11.bin  (32KB, BG tiles parte 0)\n");
    printf("  --twc9  <f>  twc-9.bin   (32KB, BG tiles parte 1)\n\n");
    printf("Controles:\n");
    printf("  Flechas        Trackball P1\n");
    printf("  W/A/S/D        Trackball P2\n");
    printf("  Ctrl/Espacio   Disparo P1\n");
    printf("  RCtrl/Enter    Disparo P2\n");
    printf("  1 / 2          Start P1 / P2\n");
    printf("  5 / 6          Coin 1 / Coin 2\n");
    printf("  F5             Refrescar pantalla\n");
    printf("  Escape         Salir\n");
}

int main(int argc, char* argv[]) {
    static TehkanWC t;
    tehkanwc_init(&t);

    const char *twc1="roms/tehkanwc/twc-1.bin", *twc2="roms/tehkanwc/twc-2.bin", *twc3="roms/tehkanwc/twc-3.bin";
    const char *twc4="roms/tehkanwc/twc-4.bin", *twc6="roms/tehkanwc/twc-6.bin", *twc12="roms/tehkanwc/twc-12.bin";
    const char *twc8="roms/tehkanwc/twc-8.bin", *twc7="roms/tehkanwc/twc-7.bin", *twc11="roms/tehkanwc/twc-11.bin", *twc9="roms/tehkanwc/twc-9.bin";
#if 0
    for (int i = 1; i < argc; i++) {
#define ARG(f,v) if(!strcmp(argv[i],f)&&i+1<argc){v=argv[++i];continue;}
        ARG("--twc1",  twc1)  ARG("--twc2",  twc2)  ARG("--twc3",  twc3)
        ARG("--twc4",  twc4)  ARG("--twc6",  twc6)  ARG("--twc12", twc12)
        ARG("--twc8",  twc8)  ARG("--twc7",  twc7)
        ARG("--twc11", twc11) ARG("--twc9",  twc9)
        if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) {
            print_usage(argv[0]); tehkanwc_destroy(&t); return 0;
        }
#undef ARG
    }
#endif
    if (argc < 2) { 
		print_usage(argv[0]); 
		//tehkanwc_destroy(&t); 
		//return 0; 
	}

    /*if (twc1)*/  tehkanwc_load_mainrom(&t, twc1,  0x0000, 0x4000);
    /*if (twc2)*/  tehkanwc_load_mainrom(&t, twc2,  0x4000, 0x4000);
    /*if (twc3)*/  tehkanwc_load_mainrom(&t, twc3,  0x8000, 0x4000);
    /*if (twc4)*/  tehkanwc_load_subrom(&t, twc4);
    /*if (twc6)*/  tehkanwc_load_sndrom(&t, twc6);
    /*if (twc12)*/ tehkanwc_load_gfx1(&t, twc12);
    /*if (twc8)*/  tehkanwc_load_gfx2(&t, twc8,  0x0000, 0x8000);
    /*if (twc7)*/  tehkanwc_load_gfx2(&t, twc7,  0x8000, 0x8000);
    /*if (twc11)*/ tehkanwc_load_gfx3(&t, twc11, 0x0000, 0x8000);
    /*if (twc9)*/  tehkanwc_load_gfx3(&t, twc9,  0x8000, 0x8000);

    tehkanwc_decode_gfx(&t);

    //if (!t.have_mainrom)
    //    fprintf(stderr, "Aviso: ROM principal no cargada.\n");

    // Verificar NMI disponible en el core
    // (si no existe z80_pulse_nmi, compilar sin ella)

    const uint32_t FRAME_MS = 1000 / TWC_FPS;
    printf("Tehkan World Cup - iniciando. ESC para salir.\n");

    while (!t.quit) {
        uint32_t t0 = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) t.quit = true;
            else if (e.type == SDL_KEYDOWN) tehkanwc_handle_key(&t, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)   tehkanwc_handle_key(&t, e.key.keysym.scancode, false);
        }
        tehkanwc_run_frame(&t);
        if (t.turbo_mode) {
            if ((t.frame_counter & 7) == 0) tehkanwc_render(&t);
        } else {
            tehkanwc_render(&t);
            uint32_t el = SDL_GetTicks() - t0;
            if (el < FRAME_MS) SDL_Delay(FRAME_MS - el);
        }
    }

    tehkanwc_destroy(&t);
    return 0;
}