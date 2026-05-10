/*
 * minivadr.c  -  Emulador de Minivader (Taito, 1990)
 *
 * Estructura y estilo basados en zx.c del proyecto zxtiny (chusogar/zxtiny).
 * Driver original: MAME 0.37b7 drivers/minivadr.c  (Takahiro Nogi, 1999)
 *
 * Hardware documentado:
 *   CPU    Z80 @ 4 MHz (24 MHz / 6), modo interrupcion IM1
 *   ROM    8 KB en 0x0000-0x1FFF  fichero: d26-01.bin
 *   RAM    8 KB en 0xA000-0xBFFF  (VRAM incluida)
 *   Video  256x256, 1bpp; cada byte = 8 px (bit7=px izquierdo)
 *          VRAM offset = y*32 + (x/8), area visible y=[16,239]
 *   Input  lectura en 0xE008, bits active-low:
 *            bit0=izquierda  bit1=derecha  bit2=disparo  bit3=moneda
 *   IRQ    IM1 en VBLANK (60 Hz); vector 0xFF -> RST 38h
 *   Sound  ninguno
 */

#include "minivadr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Acceso a memoria
// ---------------------------------------------------------------------------

static inline uint8_t* minivadr_ptr(Minivader* m, uint16_t addr) {
    if (addr <= MINIVADR_ROM_END)
        return &m->rom[addr];
    if (addr >= MINIVADR_RAM_START && addr <= MINIVADR_RAM_END)
        return &m->ram[addr - MINIVADR_RAM_START];
    return NULL;
}

// ---------------------------------------------------------------------------
// Render: actualiza el framebuffer a partir de la VRAM
// Se llama desde minivadr_render() (full refresh) y desde mem_write (incremental)
// ---------------------------------------------------------------------------

// Dibuja un byte de VRAM en el framebuffer.
// offset = posicion dentro de la VRAM (0..0x1FFF)
static void minivadr_draw_byte(Minivader* m, uint16_t vram_offset, uint8_t data) {
    int y = vram_offset / MINIVADR_BYTES_PER_LINE;
    int x = (vram_offset % MINIVADR_BYTES_PER_LINE) * 8;

    // Solo area visible
    if (y < MINIVADR_VIS_Y0 || y > MINIVADR_VIS_Y1) return;
    if (x < 0 || x >= MINIVADR_SCREEN_W) return;

    int fb_y = y - MINIVADR_VIS_Y0;
    uint32_t* dst = &m->framebuffer[fb_y * MINIVADR_SCREEN_W + x];

    // bit7 = pixel mas a la izquierda (mismo que MAME: x + (7-i) con bit i)
    dst[0] = (data & 0x80) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[1] = (data & 0x40) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[2] = (data & 0x20) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[3] = (data & 0x10) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[4] = (data & 0x08) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[5] = (data & 0x04) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[6] = (data & 0x02) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
    dst[7] = (data & 0x01) ? MINIVADR_COL_WHITE : MINIVADR_COL_BLACK;
}

// ---------------------------------------------------------------------------
// Callbacks Z80
// ---------------------------------------------------------------------------

static uint8_t mem_read(void* userdata, uint16_t addr) {
    Minivader* m = (Minivader*)userdata;

    // INPUTS: el juego lee los controles mediante memoria mapeada en 0xE008
    // (ver mapa de memoria del driver MAME: AM_RANGE(0xe008,0xe008) AM_READ(input_port_0_r)).
    if (addr == MINIVADR_INPUT_ADDR)
        return m->input;

    uint8_t* p = minivadr_ptr(m, addr);
    return p ? *p : 0xFF;
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    Minivader* m = (Minivader*)userdata;

    // ROM: solo lectura
    if (addr <= MINIVADR_ROM_END) return;

    // VRAM / RAM: 0xA000-0xBFFF
    if (addr >= MINIVADR_RAM_START && addr <= MINIVADR_RAM_END) {
        uint16_t off = addr - MINIVADR_RAM_START;
        m->ram[off] = val;
        // Actualizacion incremental del framebuffer (igual que minivadr_videoram_w en MAME)
        minivadr_draw_byte(m, off, val);
        return;
    }

    // 0xE008: NOP (escritura ignorada segun driver MAME: MWA_NOP)
    if (addr == MINIVADR_INPUT_ADDR) return;
}

static uint8_t port_in(z80* z, uint16_t port) {
    Minivader* m = (Minivader*)z->userdata;

    // Minivader lee los controles via memoria mapeada (0xE008).
    // Este core lo implementa en mem_read(); aqui devolvemos lo mismo por tolerancia
    // si algun codigo ejecutase un IN a 0xE008.
    if (port == MINIVADR_INPUT_ADDR)
        return m->input;

    // Cualquier otro puerto devuelve 0xFF (bus flotante)
    return 0xFF;
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z; (void)port; (void)val;
    // Minivader no usa puertos de salida significativos
}

// ---------------------------------------------------------------------------
// minivadr_init
// ---------------------------------------------------------------------------

void minivadr_init(Minivader* m) {
    memset(m, 0, sizeof(*m));

    // Input: todos los bits a 1 (active-low = sin pulsar)
    m->input = 0xFF;

    // CPU
    z80_init(&m->cpu);
    m->cpu.userdata   = m;
    m->cpu.read_byte  = mem_read;
    m->cpu.write_byte = mem_write;
    m->cpu.port_in    = port_in;
    m->cpu.port_out   = port_out;

    // SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    m->window = SDL_CreateWindow(
        "Minivader (Taito 1990)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        MINIVADR_SCREEN_W * MINIVADR_SCALE,
        MINIVADR_VIS_H    * MINIVADR_SCALE,
        SDL_WINDOW_SHOWN
    );

    m->renderer = SDL_CreateRenderer(
        m->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    // Textura del framebuffer: solo la zona visible
    m->texture = SDL_CreateTexture(
        m->renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        MINIVADR_SCREEN_W,
        MINIVADR_VIS_H
    );

    SDL_RenderSetLogicalSize(
        m->renderer,
        MINIVADR_SCREEN_W * MINIVADR_SCALE,
        MINIVADR_VIS_H    * MINIVADR_SCALE
    );

    // Framebuffer inicial: negro
    for (int i = 0; i < MINIVADR_SCREEN_W * MINIVADR_VIS_H; i++)
        m->framebuffer[i] = MINIVADR_COL_BLACK;
}

// ---------------------------------------------------------------------------
// minivadr_destroy
// ---------------------------------------------------------------------------

void minivadr_destroy(Minivader* m) {
    if (m->texture)  SDL_DestroyTexture(m->texture);
    if (m->renderer) SDL_DestroyRenderer(m->renderer);
    if (m->window)   SDL_DestroyWindow(m->window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// minivadr_load_rom
// ---------------------------------------------------------------------------

int minivadr_load_rom(Minivader* m, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path);
        return -1;
    }
    size_t n = fread(m->rom, 1, MINIVADR_ROM_SIZE, f);
    fclose(f);
    if (n != MINIVADR_ROM_SIZE) {
        fprintf(stderr, "[ROM] '%s': esperados %d bytes, leidos %d\n",
                path, MINIVADR_ROM_SIZE, (int)n);
        return -1;
    }
    printf("[ROM] '%s' cargada (%d bytes)\n", path, (int)n);
    return 0;
}

// ---------------------------------------------------------------------------
// minivadr_run_frame
// Ejecuta un frame completo (66666 ciclos a 4 MHz / 60 Hz).
// La IRQ VBLANK se lanza una vez al inicio del frame (IM1, vector 0xFF).
// ---------------------------------------------------------------------------

void minivadr_run_frame(Minivader* m) {
    // Ejecutar un frame completo de CPU (66666 ciclos a 4 MHz / 60 Hz)
    // MAME: interrupt, 1  ->  una IRQ IM1 (vector 0xFF -> RST 38h) por VBLANK
    // La lanzamos al final del frame, justo antes de renderizar,
    // igual que el hardware real que interrumpe al terminar el barrido.
    z80_step_n(&m->cpu, MINIVADR_CYCLES_PER_FRAME);
    z80_pulse_irq(&m->cpu, 0xFF);
    m->frame_counter++;
}

// ---------------------------------------------------------------------------
// minivadr_render
// ---------------------------------------------------------------------------

void minivadr_render(Minivader* m) {
    SDL_UpdateTexture(
        m->texture, NULL,
        m->framebuffer,
        MINIVADR_SCREEN_W * (int)sizeof(uint32_t)
    );
    SDL_RenderClear(m->renderer);
    SDL_Rect dst = {
        0, 0,
        MINIVADR_SCREEN_W * MINIVADR_SCALE,
        MINIVADR_VIS_H    * MINIVADR_SCALE
    };
    SDL_RenderCopy(m->renderer, m->texture, NULL, &dst);
    SDL_RenderPresent(m->renderer);
}

// ---------------------------------------------------------------------------
// minivadr_handle_key
// Mapeo de teclado -> bits de input (active-low)
//   bit0 = LEFT   <- flecha izquierda / Z
//   bit1 = RIGHT  <- flecha derecha  / X
//   bit2 = FIRE   <- espacio / LCTRL
//   bit3 = COIN   <- 5
// ---------------------------------------------------------------------------

void minivadr_handle_key(Minivader* m, SDL_Scancode sc, bool pressed) {
    uint8_t bit = 0;
    bool valid = true;

    switch (sc) {
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_Z:
        bit = 0x01; break;
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_X:
        bit = 0x02; break;
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        bit = 0x04; break;
    case SDL_SCANCODE_5:
        bit = 0x08; break;
    case SDL_SCANCODE_F12:
        // Full refresh del framebuffer
        if (pressed) {
            for (int off = 0; off < MINIVADR_RAM_SIZE; off++)
                minivadr_draw_byte(m, (uint16_t)off, m->ram[off]);
        }
        valid = false; break;
    case SDL_SCANCODE_ESCAPE:
        if (pressed) m->quit = true;
        valid = false; break;
    default:
        valid = false; break;
    }

    if (valid) {
        if (pressed)
            m->input &= ~bit;  // active-low: pulsar = poner a 0
        else
            m->input |= bit;   // soltar = poner a 1
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    const char* rom_path = "d26-01.bin";

    // Parsear argumentos simples
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Uso: %s [d26-01.bin]\n", argv[0]);
            printf("\nEmulador de Minivader (Taito, 1990)\n");
            printf("ROM requerida: d26-01.bin (8KB, CRC a96c823d)\n\n");
            printf("Controles:\n");
            printf("  Flecha izquierda / Z  - Mover izquierda\n");
            printf("  Flecha derecha  / X   - Mover derecha\n");
            printf("  Espacio / Ctrl        - Disparar\n");
            printf("  5                     - Insertar moneda\n");
            printf("  F12                   - Refrescar pantalla\n");
            printf("  Escape                - Salir\n");
            return 0;
        }
        // Primer argumento no-flag se trata como ruta de ROM
        if (argv[i][0] != '-')
            rom_path = argv[i];
    }

    static Minivader m;
    minivadr_init(&m);

    if (minivadr_load_rom(&m, rom_path) != 0) {
        fprintf(stderr, "ERROR: No se pudo cargar la ROM '%s'\n", rom_path);
        fprintf(stderr, "       Coloca 'd26-01.bin' en el directorio actual.\n");
        minivadr_destroy(&m);
        return 1;
    }

    // Frame timing: 60 Hz (16 ms/frame)
    const uint32_t FRAME_MS = 1000 / MINIVADR_FPS;

    printf("Minivader - iniciando emulacion. ESC para salir.\n");

    while (!m.quit) {
        uint32_t t0 = SDL_GetTicks();

        // Eventos SDL
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                m.quit = true;
            else if (e.type == SDL_KEYDOWN)
                minivadr_handle_key(&m, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)
                minivadr_handle_key(&m, e.key.keysym.scancode, false);
        }

        minivadr_run_frame(&m);

        if (m.turbo_mode) {
            if ((m.frame_counter & 7) == 0)
                minivadr_render(&m);
        } else {
            minivadr_render(&m);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS)
                SDL_Delay(FRAME_MS - elapsed);
        }
    }

    minivadr_destroy(&m);
    return 0;
}
