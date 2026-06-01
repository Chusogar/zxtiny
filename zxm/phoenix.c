/*
 * phoenix.c  -  Emulador de Phoenix (Amstar, 1980)
 *
 * Estructura y estilo basados en zx.c / minivadr.c del proyecto zxtiny.
 * Driver original: MAME 0.37b7 drivers/phoenix.c + vidhrdw/phoenix.c
 *                  (Richard Davies)
 *
 * Hardware emulado:
 *   CPU     Intel 8080 @ 3.072 MHz, sin IRQs (ignore_interrupt en MAME)
 *   ROM     16 KB en 0x0000-0x3FFF
 *   RAM     4 KB paginada (2 paginas) en 0x4000-0x4FFF
 *   Video   Tilemap 32x32 tiles 8x8px, 2bpp, ROT90 CCW
 *   Paleta  2 PROMs 256x4, espacio 2R-2G-2B
 *   Input   0x7000 active-low, DSW 0x7800 (bit7=VBLANK)
 *
 * Compilar (ejemplo):
 *   gcc phoenix.c z80/jgz80/z80.c -o phoenix -lSDL2 -O2
 *
 * Carga (ejemplo con ROMs sueltas):
 *   ./phoenix --ic45 phoenix/ic45 --ic46 phoenix/ic46 --ic47 phoenix/ic47 --ic48 phoenix/ic48 \
 *             --ic49 phoenix/ic49 --ic50 phoenix/ic50 --ic51 phoenix/ic51 --ic52 phoenix/ic52 \
 *             --ic23 phoenix/ic23 --ic24 phoenix/ic24 --ic39 phoenix/ic39 --ic40 phoenix/ic40 \
 *             --prom-lo phoenix/ic40_b.bin --prom-hi phoenix/ic41_a.bin
 */

#include "phoenix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Utilidades de carga
// ---------------------------------------------------------------------------

static int load_file(uint8_t* dst, int max_size, const char* path, int offset, int len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    int to_read = (len > 0) ? len : (int)fsz;
    if (offset + to_read > max_size) {
        fprintf(stderr, "[ROM] '%s': desbordamiento (off=%d len=%d max=%d)\n",
                path, offset, to_read, max_size);
        fclose(f);
        return -1;
    }

    int n = (int)fread(dst + offset, 1, (size_t)to_read, f);
    fclose(f);

    if (n != to_read) {
        fprintf(stderr, "[ROM] '%s': leidos %d de %d bytes\n", path, n, to_read);
        return -1;
    }

    printf("[ROM] '%s' -> offset 0x%04X, %d bytes\n", path, offset, n);
    return 0;
}

// ---------------------------------------------------------------------------
// Decodificacion de GFX
// ---------------------------------------------------------------------------
// Layout MAME (charlayout):
// 256 tiles, 2bpp
// planes separados: plano0 en bytes 0..0x7FF (256 tiles x 8 filas x 1 byte)
//                   plano1 en bytes 0x800..0xFFF
// Dentro de cada tile: 8 bytes consecutivos, uno por fila
// IMPORTANTE: en tu set, bit0 = pixel izquierdo (si se usa 7-col queda espejado).
static void decode_charset(const uint8_t* rom, PhxGfx* gfx) {
    for (int t = 0; t < PHX_NUM_TILES; t++) {
        for (int row = 0; row < PHX_TILE_H; row++) {
            uint8_t b0 = rom[t * 8 + row];            // plano 0
            uint8_t b1 = rom[0x800 + t * 8 + row];    // plano 1
            for (int col = 0; col < PHX_TILE_W; col++) {
                int bit = col;  // bit0 = pixel izquierdo (evita espejo horizontal)
                uint8_t p = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
                gfx->pix[t][row][col] = p;
            }
        }
    }
}

void phoenix_decode_gfx(Phoenix* p) {
    decode_charset(p->gfx1_rom, &p->gfx1);
    decode_charset(p->gfx2_rom, &p->gfx2);
}

// ---------------------------------------------------------------------------
// Paleta
// ---------------------------------------------------------------------------

void phoenix_build_palette(Phoenix* p) {
    // Las dos PROMs juntas dan 256 entradas
    // prom_lo[i] = bits 0-3 del color i, prom_hi[i] = bits 0-3 del color i+128
    // Procesamos 128 (2 bancos x 64)
    for (int i = 0; i < PHX_NUM_COLORS; i++) {
        uint8_t lo = p->prom_lo[i];
        uint8_t hi = p->prom_hi[i];

        uint8_t r = (uint8_t)(0x55 * (lo & 1) + 0xAA * (hi & 1));
        uint8_t g = (uint8_t)(0x55 * ((lo >> 2) & 1) + 0xAA * ((hi >> 2) & 1));
        uint8_t b = (uint8_t)(0x55 * ((lo >> 1) & 1) + 0xAA * ((hi >> 1) & 1));

        p->palette[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ---------------------------------------------------------------------------
// Lookup de color de tile (simplificacion practica)
// ---------------------------------------------------------------------------

static inline uint32_t tile_color(Phoenix* p, int gfx, int code, int pixel) {
    int group = (code >> 5) & 7;
    int base  = (p->palette_bank * 64) + (gfx == 2 ? 32 : 0);
    // Nota: esta indexacion es una aproximacion simple y usable
    int idx   = base + group + pixel * 8;

    if (idx < 0 || idx >= PHX_NUM_COLORS)
        return 0xFF000000u;
    return p->palette[idx];
}

// ---------------------------------------------------------------------------
// Callbacks Z80 (emulando 8080)
// ---------------------------------------------------------------------------

static uint8_t mem_read(void* userdata, uint16_t addr) {
    Phoenix* p = (Phoenix*)userdata;

    if (addr <= PHX_ROM_END) {
        return p->rom[addr];
    }

    if (addr >= PHX_RAM_START && addr <= PHX_RAM_END) {
        uint16_t off = addr - PHX_RAM_START;
        return p->ram[p->ram_page & 1][off];
    }

    // Registros mapeados en memoria (lectura)
    if (addr >= PHX_INPUT_START && addr <= PHX_INPUT_END) {
        return p->input;
    }
    if (addr >= PHX_DSW_START && addr <= PHX_DSW_END) {
        return p->dsw;
    }

    return 0xFF;
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    Phoenix* p = (Phoenix*)userdata;

    // RAM paginada
    if (addr >= PHX_RAM_START && addr <= PHX_RAM_END) {
        uint16_t off = addr - PHX_RAM_START;
        p->ram[p->ram_page & 1][off] = val;

        // Marcar dirty BG cuando se escribe en videoram BG
        // (solo si estamos en la pagina que contiene BG)
        // En este ejemplo marcamos ambos rangos por simplicidad si caen dentro.
        if (off >= PHX_BG_VRAM_OFF && off < (PHX_BG_VRAM_OFF + PHX_VRAM_SIZE)) {
            p->dirty_bg[off - PHX_BG_VRAM_OFF] = true;
        }
        return;
    }

    // Video register (0x5000)
    if (addr >= PHX_VIDEOREG_START && addr <= PHX_VIDEOREG_END) {
        // bit0 = pagina RAM, bit1 = banco paleta
        p->ram_page     = val & 1;
        p->palette_bank = (val >> 1) & 1;
        return;
    }

    // Scroll (0x5800)
    if (addr >= PHX_SCROLL_START && addr <= PHX_SCROLL_END) {
        p->bg_scroll = val;
        return;
    }

    // Sonido (no implementado)
    (void)val;
}

static uint8_t port_in(z80* z, uint16_t port) {
    (void)z; (void)port;
    return 0xFF;
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z; (void)port; (void)val;
}

// ---------------------------------------------------------------------------
// phoenix_init / destroy
// ---------------------------------------------------------------------------

void phoenix_init(Phoenix* p) {
    memset(p, 0, sizeof(*p));

    // Input en hardware es active-low: reposo todo a 1
    p->input = 0xFF;
    p->dsw   = 0xFF;

    // Dirty BG a true para forzar primer render
    for (int i = 0; i < PHX_VRAM_SIZE; i++) p->dirty_bg[i] = true;

    // Z80 core en modo 8080 (en jgz80 normalmente hay flag/ajuste interno; aquí solo inicializamos)
    z80_init(&p->cpu);
    p->cpu.userdata   = p;
    p->cpu.read_byte  = mem_read;
    p->cpu.write_byte = mem_write;
    p->cpu.port_in    = port_in;
    p->cpu.port_out   = port_out;
}

void phoenix_destroy(Phoenix* p) {
    if (p->texture)  SDL_DestroyTexture(p->texture);
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window)   SDL_DestroyWindow(p->window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Carga de ROMs
// ---------------------------------------------------------------------------

int phoenix_load_rom(Phoenix* p, const char* path) {
    int r = load_file(p->rom, PHX_ROM_SIZE, path, 0, PHX_ROM_SIZE);
    if (r == 0) p->have_rom = true;
    return r;
}

int phoenix_load_rom_chunk(Phoenix* p, const char* path, int offset, int size) {
    int r = load_file(p->rom, PHX_ROM_SIZE, path, offset, size);
    if (r == 0) p->have_rom = true;
    return r;
}

int phoenix_load_gfx(Phoenix* p, int gfx, const char* path, int offset, int len) {
    uint8_t* dst = (gfx == 1) ? p->gfx1_rom : p->gfx2_rom;
    int r = load_file(dst, PHX_GFX_ROM_SIZE, path, offset, len);
    if (r == 0) {
        if (gfx == 1) p->have_gfx1 = true;
        if (gfx == 2) p->have_gfx2 = true;
    }
    return r;
}

int phoenix_load_prom(Phoenix* p, int which, const char* path) {
    uint8_t* dst = (which == 0) ? p->prom_lo : p->prom_hi;
    int r = load_file(dst, PHX_PROM_SIZE, path, 0, PHX_PROM_SIZE);
    if (r == 0) p->have_proms = true;
    return r;
}

// ---------------------------------------------------------------------------
// Renderizado
// ---------------------------------------------------------------------------

// Dibuja un tile en un buffer (sin rotar)
static void draw_tile_buf(Phoenix* p, uint32_t* buf, int gfx, int code, int sx, int sy, bool transparent) {
    PhxGfx* charset = (gfx == 1) ? &p->gfx1 : &p->gfx2;

    for (int row = 0; row < PHX_TILE_H; row++) {
        int py = sy + row;
        if ((unsigned)py >= (unsigned)PHX_LOG_H) continue;

        uint32_t* dst = &buf[py * PHX_LOG_W + sx];

        for (int col = 0; col < PHX_TILE_W; col++) {
            int px = sx + col;
            if ((unsigned)px >= (unsigned)PHX_LOG_W) continue;

            uint8_t pix = charset->pix[code & 0xFF][row][col];

            if (transparent && pix == 0) {
                // alpha=0
                dst[col] = 0x00000000u;
            } else {
                dst[col] = tile_color(p, gfx, code, pix);
            }
        }
    }
}

// Renderiza BG en logbuf (sin rotar) - solo tiles dirty
static void render_bg(Phoenix* p) {
    // BG vive en ram[?] + PHX_BG_VRAM_OFF
    // NOTA: el uso exacto de pagina BG/FG puede variar por set; aquí usamos la pagina activa
    // para leer VRAM, como simplificación.
    uint8_t* vram = &p->ram[p->ram_page & 1][PHX_BG_VRAM_OFF];

    for (int offs = 0; offs < PHX_VRAM_SIZE; offs++) {
        if (!p->dirty_bg[offs]) continue;
        p->dirty_bg[offs] = false;

        int code = vram[offs];
        int sx = (offs % PHX_TILES_X) * PHX_TILE_W;
        int sy = (offs / PHX_TILES_X) * PHX_TILE_H;

        draw_tile_buf(p, p->logbuf, 1, code, sx, sy, false);
    }
}

// Renderiza FG en fgbuf (sin rotar), con transparencia pen0
static void render_fg(Phoenix* p) {
    uint8_t* vram = &p->ram[p->ram_page & 1][PHX_FG_VRAM_OFF];

    // Limpiar fgbuf
    memset(p->fgbuf, 0, sizeof(p->fgbuf));

    for (int offs = 0; offs < PHX_VRAM_SIZE; offs++) {
        int code = vram[offs];
        int sx = (offs % PHX_TILES_X) * PHX_TILE_W;
        int sy = (offs / PHX_TILES_X) * PHX_TILE_H;

        draw_tile_buf(p, p->fgbuf, 2, code, sx, sy, true);
    }
}

// Aplica ROT90 CCW del logbuf al framebuffer y el scroll del BG.
// ROT90 CCW inverse mapping:
//   src_x = W-1-oy
//   src_y = ox
// donde W = PHX_LOG_W = 256, H = PHX_LOG_H = 208
// screen: W=208, H=256
// bg_scroll: desplazamiento en X del logbuf (en pixels), aplicado como wrap-around
static void apply_rot_and_scroll(Phoenix* p) {
    /*
     * Phoenix (MAME) rota la pantalla 90º CCW. Nosotros dibujamos primero en un
     * buffer sin rotar (logbuf/fgbuf) de 256x208 y aquí:
     *   - aplicamos el scroll del BG (registro 0x5800)
     *   - rotamos 90º CCW al framebuffer final 208x256
     *
     * Rotación 90º CCW (dst->src):
     *   src_x = dst_y
     *   src_y = (H - 1) - dst_x
     * con W=256, H=208.
     */

    // MAME: scroll = -phoenix_scroll (8-bit wrap)
    const int scroll = (-((int)p->bg_scroll)) & 0xFF;

    for (int oy = 0; oy < PHX_SCREEN_H; oy++) {            // dst_y: 0..255
        const int src_x_noscroll = oy & 0xFF;              // 0..255
        const int src_x_bg       = (src_x_noscroll + scroll) & 0xFF;

        for (int ox = 0; ox < PHX_SCREEN_W; ox++) {        // dst_x: 0..207
            const int src_y = (PHX_LOG_H - 1) - ox;        // 207..0

            const uint32_t bg = p->logbuf[src_y * PHX_LOG_W + src_x_bg];
            const uint32_t fg = p->fgbuf[src_y * PHX_LOG_W + src_x_noscroll];

            // Componer (FG pen0 transparente => alpha=0)
            p->framebuffer[oy * PHX_SCREEN_W + ox] = (fg & 0xFF000000u) ? fg : bg;
        }
    }
}


// ---------------------------------------------------------------------------
// phoenix_run_frame / phoenix_render
// ---------------------------------------------------------------------------

void phoenix_run_frame(Phoenix* p) {
    // Phoenix usa ignore_interrupt: la CPU corre libre sin IRQs.
    // El VBLANK lo simulamos togglando bit7 del DSW cada frame.
    // El juego sondea 0x7800 bit7 en bucle esperando VBLANK (active-low).

    // Ejecutar CPU un frame
    int cycles = PHX_CYCLES_PER_FRAME;
    while (cycles > 0) {
        int ran = z80_step(&p->cpu);
        if (ran <= 0) ran = 1;
        cycles -= ran;
    }

    // Toggle VBLANK (bit7 active-low)
    p->frame_counter++;
    if ((p->frame_counter & 1) == 0) {
        p->vblank = true;
        p->dsw &= ~0x80;  // bit7=0 durante VBLANK
    } else {
        p->vblank = false;
        p->dsw |= 0x80;   // bit7=1 fuera de VBLANK
    }
}

void phoenix_render(Phoenix* p) {
    render_bg(p);
    render_fg(p);
    apply_rot_and_scroll(p);

    // Subir a SDL
    SDL_UpdateTexture(p->texture, NULL, p->framebuffer, PHX_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(p->renderer);
    SDL_RenderCopy(p->renderer, p->texture, NULL, NULL);
    SDL_RenderPresent(p->renderer);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void set_input_bit(Phoenix* p, uint8_t mask, bool pressed) {
    // active-low: pressed => 0
    if (pressed) p->input &= (uint8_t)~mask;
    else         p->input |= mask;
}

void phoenix_handle_key(Phoenix* p, SDL_Scancode sc, bool pressed) {
    switch (sc) {
        case SDL_SCANCODE_LEFT:   set_input_bit(p, PHX_IN_LEFT,   pressed); break;
        case SDL_SCANCODE_RIGHT:  set_input_bit(p, PHX_IN_RIGHT,  pressed); break;
        case SDL_SCANCODE_SPACE:
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:  set_input_bit(p, PHX_IN_FIRE,   pressed); break;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:   set_input_bit(p, PHX_IN_SHIELD, pressed); break;
        case SDL_SCANCODE_1:      set_input_bit(p, PHX_IN_START1, pressed); break;
        case SDL_SCANCODE_2:      set_input_bit(p, PHX_IN_START2, pressed); break;
        case SDL_SCANCODE_5:      set_input_bit(p, PHX_IN_COIN,   pressed); break;

        case SDL_SCANCODE_ESCAPE:
            if (pressed) p->quit = true;
            break;

        case SDL_SCANCODE_F5:
            if (pressed) {
                for (int i = 0; i < PHX_VRAM_SIZE; i++) p->dirty_bg[i] = true;
            }
            break;

        case SDL_SCANCODE_TAB:
            if (pressed) p->turbo_mode = !p->turbo_mode;
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// main / CLI
// ---------------------------------------------------------------------------

static void print_usage(const char* exe) {
    printf("Uso: %s [opciones]\n\n", exe);
    printf("Emulador de Phoenix (Amstar, 1980)\n\n");
    printf("Opciones:\n");
    printf(" --rom ROM             ROM de programa unica de 16KB (concatenada)\n");
    printf(" --ic45..--ic52 FILE   ROMs de programa individuales (2KB cada una)\n");
    printf(" --ic23 FILE --ic24 FILE   GFX1 background charset (2KB cada una)\n");
    printf(" --ic39 FILE --ic40 FILE   GFX2 foreground charset (2KB cada una)\n");
    printf(" --prom-lo FILE        Color PROM low (ic40_b.bin, 256 bytes)\n");
    printf(" --prom-hi FILE        Color PROM high (ic41_a.bin, 256 bytes)\n\n");
    printf("Controles:\n");
    printf(" Flechas: mover\n");
    printf(" Espacio/Ctrl: disparar\n");
    printf(" Alt: escudo\n");
    printf(" 1/2: start\n");
    printf(" 5: moneda\n");
    printf(" TAB: turbo\n");
    printf(" F5: refrescar BG\n");
    printf(" ESC: salir\n");
}

static int sdl_init_video(Phoenix* p) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return -1;
    }

    int win_w = PHX_SCREEN_W * PHX_SCALE;
    int win_h = PHX_SCREEN_H * PHX_SCALE;

    p->window = SDL_CreateWindow("Phoenix (Amstar, 1980)",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                win_w, win_h, SDL_WINDOW_SHOWN);
    if (!p->window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return -1;
    }

    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_ACCELERATED);
    if (!p->renderer) {
        fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(p->renderer, PHX_SCREEN_W, PHX_SCREEN_H);

    p->texture = SDL_CreateTexture(p->renderer,
                                   SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   PHX_SCREEN_W, PHX_SCREEN_H);
    if (!p->texture) {
        fprintf(stderr, "SDL_CreateTexture error: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    static Phoenix p;
    phoenix_init(&p);

    // Parse CLI
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            print_usage(argv[0]);
            return 0;
        }
        else if (!strcmp(a, "--rom") && i + 1 < argc) {
            phoenix_load_rom(&p, argv[++i]);
        }
        /*else if (!strcmp(a, "--ic45") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic45", 0x0000, 0x0800);
        /*else if (!strcmp(a, "--ic46") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic46", 0x0800, 0x0800);
        /*else if (!strcmp(a, "--ic47") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic47", 0x1000, 0x0800);
        /*else if (!strcmp(a, "--ic48") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic48", 0x1800, 0x0800);
        /*else if (!strcmp(a, "--ic49") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic49", 0x2000, 0x0800);
        /*else if (!strcmp(a, "--ic50") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic50", 0x2800, 0x0800);
        /*else if (!strcmp(a, "--ic51") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic51", 0x3000, 0x0800);
        /*else if (!strcmp(a, "--ic52") && i + 1 < argc)*/ phoenix_load_rom_chunk(&p, "roms/phoenix/ic52", 0x3800, 0x0800);

        /*else if (!strcmp(a, "--ic23") && i + 1 < argc)*/ phoenix_load_gfx(&p, 1, "roms/phoenix/ic23", 0x0000, 0x0800);
        /*else if (!strcmp(a, "--ic24") && i + 1 < argc)*/ phoenix_load_gfx(&p, 1, "roms/phoenix/ic24", 0x0800, 0x0800);

        /*else if (!strcmp(a, "--ic39") && i + 1 < argc)*/ phoenix_load_gfx(&p, 2, "roms/phoenix/ic39", 0x0000, 0x0800);
        /*else if (!strcmp(a, "--ic40") && i + 1 < argc)*/ phoenix_load_gfx(&p, 2, "roms/phoenix/ic40", 0x0800, 0x0800);

        /*else if (!strcmp(a, "--prom-lo") && i + 1 < argc)*/ phoenix_load_prom(&p, 0, "roms/phoenix/ic40_b.bin");
        /*else if (!strcmp(a, "--prom-hi") && i + 1 < argc)*/ phoenix_load_prom(&p, 1, "roms/phoenix/ic41_a.bin");

        /*else {
            fprintf(stderr, "Opcion desconocida: %s\n", a);
            print_usage(argv[0]);
            return 1;
        }*/
    }

    /*if (!p.have_rom || !p.have_gfx1 || !p.have_gfx2 || !p.have_proms) {
        fprintf(stderr, "\nFaltan ROMs/GFX/PROMs. Usa --help.\n");
        return 1;
    }*/

    phoenix_decode_gfx(&p);
    phoenix_build_palette(&p);

    if (sdl_init_video(&p) != 0) {
        phoenix_destroy(&p);
        return 1;
    }

    // Main loop
    const uint32_t FRAME_MS = 1000 / PHX_FPS;

    while (!p.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) p.quit = true;
            else if (e.type == SDL_KEYDOWN)  phoenix_handle_key(&p, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)    phoenix_handle_key(&p, e.key.keysym.scancode, false);
        }

        phoenix_run_frame(&p);

        if (p.turbo_mode) {
            if ((p.frame_counter & 7) == 0)
                phoenix_render(&p);
        } else {
            phoenix_render(&p);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS)
                SDL_Delay(FRAME_MS - elapsed);
        }
    }

    phoenix_destroy(&p);
    return 0;
}