/*
 * mrdo.c - Mr. Do! (Universal, 1982)
 * SDL2 + jgz80 + 2x SN76489/SN76496
 *
 * Basado en MAME 0.37b7:
 *   - mapa 0000-7fff ROM, 8000-8fff video/color RAM, 9000-90ff sprites
 *   - 9801 y 9802: dos SN76496 a 4 MHz
 *   - paleta PROM con pesos 0x2c, 0x37, 0x43, 0x59
 *   - ROT270 final y teclas traducidas para movimiento visible correcto
 */
#include "mrdo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_rom(uint8_t *dst, size_t dst_size, const char *directory,
                    const char *name, size_t offset, size_t expected)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, name);

    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[ROM] No se puede abrir %s\n", path);
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    rewind(file);

    if (size < 0 || (size_t)size != expected || offset + expected > dst_size) {
        fprintf(stderr, "[ROM] Tamano incorrecto en %s: %ld, esperado %zu\n",
                path, size, expected);
        fclose(file);
        return -1;
    }

    size_t count = fread(dst + offset, 1, expected, file);
    fclose(file);
    if (count != expected) {
        fprintf(stderr, "[ROM] Lectura incompleta de %s\n", path);
        return -1;
    }

    printf("[ROM] %-16s -> 0x%04zx (%zu bytes)\n", name, offset, count);
    return 0;
}

static inline uint8_t graphics_bit(const uint8_t *source, unsigned bit)
{
    return (uint8_t)((source[bit >> 3] >> (7 - (bit & 7))) & 1);
}

void mrdo_decode_graphics(MrDo *m)
{
    for (int set = 0; set < 2; ++set) {
        const uint8_t *source = set == 0 ? m->gfx1 : m->gfx2;
        for (int code = 0; code < MRDO_TILE_COUNT; ++code) {
            for (int y = 0; y < 8; ++y) {
                uint8_t plane0 = source[code * 8 + y];
                uint8_t plane1 = source[0x1000 + code * 8 + y];
                for (int x = 0; x < 8; ++x) {
                    m->tile_pixels[set][code][y][x] =
                        (uint8_t)(((plane0 >> x) & 1) |
                                  (((plane1 >> x) & 1) << 1));
                }
            }
        }
    }

    static const unsigned x_offset[16] = {
         3,  2,  1,  0, 11, 10,  9,  8,
        19, 18, 17, 16, 27, 26, 25, 24
    };

    for (int code = 0; code < MRDO_SPRITE_COUNT; ++code) {
        unsigned base = (unsigned)code * 64U * 8U;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                unsigned bit = base + (unsigned)y * 32U + x_offset[x];
                m->sprite_pixels[code][y][x] =
                    (uint8_t)(graphics_bit(m->gfx3, bit + 4U) |
                              (graphics_bit(m->gfx3, bit) << 1));
            }
        }
    }
}

static void build_palette(MrDo *m)
{
    uint32_t indirect[256];

    for (int i = 0; i < 256; ++i) {
        const int a1 = ((i >> 3) & 0x1c) + (i & 0x03) + 0x20;
        const int a2 = ((i >> 0) & 0x1c) + (i & 0x03);

        int bit0 = (m->proms[a1] >> 1) & 1;
        int bit1 = (m->proms[a1] >> 0) & 1;
        int bit2 = (m->proms[a2] >> 1) & 1;
        int bit3 = (m->proms[a2] >> 0) & 1;
        const uint8_t r = (uint8_t)(0x2c * bit0 + 0x37 * bit1 +
                                    0x43 * bit2 + 0x59 * bit3);

        bit0 = (m->proms[a1] >> 3) & 1;
        bit1 = (m->proms[a1] >> 2) & 1;
        bit2 = (m->proms[a2] >> 3) & 1;
        bit3 = (m->proms[a2] >> 2) & 1;
        const uint8_t g = (uint8_t)(0x2c * bit0 + 0x37 * bit1 +
                                    0x43 * bit2 + 0x59 * bit3);

        bit0 = (m->proms[a1] >> 5) & 1;
        bit1 = (m->proms[a1] >> 4) & 1;
        bit2 = (m->proms[a2] >> 5) & 1;
        bit3 = (m->proms[a2] >> 4) & 1;
        const uint8_t b = (uint8_t)(0x2c * bit0 + 0x37 * bit1 +
                                    0x43 * bit2 + 0x59 * bit3);

        indirect[i] = UINT32_C(0xff000000) |
                      ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) |
                      (uint32_t)b;
        m->palette[i] = indirect[i];
    }

    const uint8_t *lookup = &m->proms[0x40];
    for (int i = 0; i < 64; ++i) {
        int entry = i < 32 ? (lookup[i] & 0x0f) : (lookup[i & 0x1f] >> 4);
        m->palette[256 + i] = indirect[entry + ((entry & 0x0c) << 3)];
    }
}

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int length)
{
    MrDo *m = (MrDo *)userdata;
    int16_t *output = (int16_t *)stream;
    int samples = length / (int)sizeof(int16_t);

    for (int i = 0; i < samples; ++i) {
        int32_t mixed = (int32_t)sn76489_sample(&m->sn1) +
                        (int32_t)sn76489_sample(&m->sn2);
        mixed /= 2;
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        output[i] = (int16_t)mixed;
    }
}

static uint8_t cpu_read(void *userdata, uint16_t address)
{
    MrDo *m = (MrDo *)userdata;

    if (address <= 0x7fff)
        return m->mainrom[address];
    if (address >= 0x8000 && address <= 0x87ff)
        return m->bgvideoram[address - 0x8000];
    if (address >= 0x8800 && address <= 0x8fff)
        return m->fgvideoram[address - 0x8800];
    if (address >= 0x9000 && address <= 0x90ff)
        return m->spriteram[address - 0x9000];

    if (address == 0x9803) {
        uint16_t hl = m->cpu_main.hl;
        return hl < MRDO_MAINROM_SIZE ? m->mainrom[hl] : 0xff;
    }

    if (address == 0xa000) return m->in0;
    if (address == 0xa001) return m->in1;
    if (address == 0xa002) return m->dsw1;
    if (address == 0xa003) return m->dsw2;

    if (address >= 0xe000 && address <= 0xefff)
        return m->mainram[address - 0xe000];

    return 0xff;
}

static void cpu_write(void *userdata, uint16_t address, uint8_t value)
{
    MrDo *m = (MrDo *)userdata;

    if (address >= 0x8000 && address <= 0x87ff) {
        m->bgvideoram[address - 0x8000] = value;
    } else if (address >= 0x8800 && address <= 0x8fff) {
        m->fgvideoram[address - 0x8800] = value;
    } else if (address >= 0x9000 && address <= 0x90ff) {
        m->spriteram[address - 0x9000] = value;
    } else if (address == 0x9800) {
        m->flipscreen = value & 1;
    } else if (address == 0x9801) {
        if (m->audio_device) SDL_LockAudioDevice(m->audio_device);
        sn76489_write(&m->sn1, value);
        if (m->audio_device) SDL_UnlockAudioDevice(m->audio_device);
    } else if (address == 0x9802) {
        if (m->audio_device) SDL_LockAudioDevice(m->audio_device);
        sn76489_write(&m->sn2, value);
        if (m->audio_device) SDL_UnlockAudioDevice(m->audio_device);
    } else if (address >= 0xe000 && address <= 0xefff) {
        m->mainram[address - 0xe000] = value;
    } else if (address >= 0xf000 && address <= 0xf7ff) {
        m->scrollx = value;
    } else if (address >= 0xf800) {
        m->scrolly = value;
    }
}

static uint8_t cpu_port_in(z80 *cpu, uint16_t port)
{
    (void)cpu;
    (void)port;
    return 0xff;
}

static void cpu_port_out(z80 *cpu, uint16_t port, uint8_t value)
{
    (void)cpu;
    (void)port;
    (void)value;
}

int mrdo_init(MrDo *m)
{
    memset(m, 0, sizeof(*m));

    m->in0 = 0xff;
    m->in1 = 0xff;
    m->dsw1 = 0xdf;
    m->dsw2 = 0xff;

    z80_init(&m->cpu_main);
    m->cpu_main.userdata = m;
    m->cpu_main.read_byte = cpu_read;
    m->cpu_main.write_byte = cpu_write;
    m->cpu_main.port_in = cpu_port_in;
    m->cpu_main.port_out = cpu_port_out;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq = MRDO_AUDIO_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = 1;
    desired.samples = MRDO_AUDIO_SAMPLES;
    desired.callback = audio_callback;
    desired.userdata = m;

    m->audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &m->audio_spec, 0);
    if (!m->audio_device) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return -1;
    }

    sn76489_init(&m->sn1, MRDO_SOUND_CLOCK, (uint32_t)m->audio_spec.freq);
    sn76489_init(&m->sn2, MRDO_SOUND_CLOCK, (uint32_t)m->audio_spec.freq);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    m->window = SDL_CreateWindow("Mr. Do! (Universal, 1982)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        MRDO_SCREEN_W * MRDO_SCALE, MRDO_SCREEN_H * MRDO_SCALE,
        SDL_WINDOW_SHOWN);
    if (!m->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    m->renderer = SDL_CreateRenderer(m->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m->renderer)
        m->renderer = SDL_CreateRenderer(m->window, -1, SDL_RENDERER_SOFTWARE);
    if (!m->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(m->renderer, MRDO_SCREEN_W, MRDO_SCREEN_H);
    m->texture = SDL_CreateTexture(m->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, MRDO_SCREEN_W, MRDO_SCREEN_H);
    if (!m->texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(m->audio_device, 0);
    return 0;
}

void mrdo_reset(MrDo *m)
{
    memset(m->mainram, 0, sizeof(m->mainram));
    memset(m->bgvideoram, 0, sizeof(m->bgvideoram));
    memset(m->fgvideoram, 0, sizeof(m->fgvideoram));
    memset(m->spriteram, 0, sizeof(m->spriteram));

    m->flipscreen = 0;
    m->scrollx = 0;
    m->scrolly = 0;
    m->frame_counter = 0;
    z80_reset(&m->cpu_main);

    if (m->audio_device) SDL_LockAudioDevice(m->audio_device);
    sn76489_reset(&m->sn1);
    sn76489_reset(&m->sn2);
    if (m->audio_device) SDL_UnlockAudioDevice(m->audio_device);
}

void mrdo_destroy(MrDo *m)
{
    if (m->audio_device) {
        SDL_CloseAudioDevice(m->audio_device);
        m->audio_device = 0;
    }
    if (m->texture) SDL_DestroyTexture(m->texture);
    if (m->renderer) SDL_DestroyRenderer(m->renderer);
    if (m->window) SDL_DestroyWindow(m->window);
    SDL_Quit();
}

int mrdo_load_romset(MrDo *m, const char *directory)
{
    int errors = 0;

    errors += load_rom(m->mainrom, sizeof(m->mainrom), directory, "a4-01.bin", 0x0000, 0x2000) != 0;
    errors += load_rom(m->mainrom, sizeof(m->mainrom), directory, "c4-02.bin", 0x2000, 0x2000) != 0;
    errors += load_rom(m->mainrom, sizeof(m->mainrom), directory, "e4-03.bin", 0x4000, 0x2000) != 0;
    errors += load_rom(m->mainrom, sizeof(m->mainrom), directory, "f4-04.bin", 0x6000, 0x2000) != 0;
    errors += load_rom(m->gfx1, sizeof(m->gfx1), directory, "s8-09.bin", 0x0000, 0x1000) != 0;
    errors += load_rom(m->gfx1, sizeof(m->gfx1), directory, "u8-10.bin", 0x1000, 0x1000) != 0;
    errors += load_rom(m->gfx2, sizeof(m->gfx2), directory, "r8-08.bin", 0x0000, 0x1000) != 0;
    errors += load_rom(m->gfx2, sizeof(m->gfx2), directory, "n8-07.bin", 0x1000, 0x1000) != 0;
    errors += load_rom(m->gfx3, sizeof(m->gfx3), directory, "h5-05.bin", 0x0000, 0x1000) != 0;
    errors += load_rom(m->gfx3, sizeof(m->gfx3), directory, "k5-06.bin", 0x1000, 0x1000) != 0;
    errors += load_rom(m->proms, sizeof(m->proms), directory, "u02--2.bin", 0x00, 0x20) != 0;
    errors += load_rom(m->proms, sizeof(m->proms), directory, "t02--3.bin", 0x20, 0x20) != 0;
    errors += load_rom(m->proms, sizeof(m->proms), directory, "f10--1.bin", 0x40, 0x20) != 0;
    errors += load_rom(m->proms, sizeof(m->proms), directory, "j10--4.bin", 0x60, 0x20) != 0;

    if (errors)
        return -1;

    mrdo_decode_graphics(m);
    build_palette(m);
    mrdo_reset(m);
    return 0;
}

static inline void put_native_pixel(MrDo *m, int x, int y, uint32_t color)
{
    x -= MRDO_VIS_X0;
    y -= MRDO_VIS_Y0;

    if ((unsigned)x >= MRDO_NATIVE_W || (unsigned)y >= MRDO_NATIVE_H)
        return;

    if (m->flipscreen) {
        x = MRDO_NATIVE_W - 1 - x;
        y = MRDO_NATIVE_H - 1 - y;
    }

    m->native_framebuffer[y * MRDO_NATIVE_W + x] = color;
}

static void draw_tilemap(MrDo *m, const uint8_t *ram, int set,
                         int scrollx, int scrolly)
{
    for (int ty = 0; ty < 32; ++ty) {
        for (int tx = 0; tx < 32; ++tx) {
            int index = ty * 32 + tx;
            uint8_t attr = ram[index];
            int code = ram[index + 0x400] + ((attr & 0x80) << 1);
            int color = attr & 0x3f;
            int bx = (tx * 8 - scrollx) & 0xff;
            int by = (ty * 8 - scrolly) & 0xff;

            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; ++px) {
                    int pen = m->tile_pixels[set][code & 0x1ff][py][px];
                    if (pen == 0)
                        continue;
                    put_native_pixel(m, (bx + px) & 0xff, (by + py) & 0xff,
                                     m->palette[(color * 4 + pen) & 0xff]);
                }
            }
        }
    }
}

static void draw_sprites(MrDo *m)
{
    for (int off = MRDO_SPRITERAM_SIZE - 4; off >= 0; off -= 4) {
        if (m->spriteram[off + 1] == 0)
            continue;

        int code = m->spriteram[off] & 0x7f;
        int attr = m->spriteram[off + 2];
        int color = attr & 0x0f;
        bool flipx = (attr & 0x10) != 0;
        bool flipy = (attr & 0x20) != 0;
        int sx = m->spriteram[off + 3];
        int sy = 256 - m->spriteram[off + 1];

        for (int py = 0; py < 16; ++py) {
            int source_y = flipy ? 15 - py : py;
            for (int px = 0; px < 16; ++px) {
                int source_x = flipx ? 15 - px : px;
                int pen = m->sprite_pixels[code][source_y][source_x];
                if (pen == 0)
                    continue;
                put_native_pixel(m, sx + px, sy + py,
                                 m->palette[256 + color * 4 + pen]);
            }
        }
    }
}

static void rotate_rot270(MrDo *m)
{
    for (int y = 0; y < MRDO_NATIVE_H; ++y) {
        for (int x = 0; x < MRDO_NATIVE_W; ++x) {
            int dx = y;
            int dy = MRDO_NATIVE_W - 1 - x;
            m->framebuffer[dy * MRDO_SCREEN_W + dx] =
                m->native_framebuffer[y * MRDO_NATIVE_W + x];
        }
    }
}

void mrdo_render(MrDo *m)
{
    for (int i = 0; i < MRDO_NATIVE_W * MRDO_NATIVE_H; ++i)
        m->native_framebuffer[i] = m->palette[0];

    draw_tilemap(m, m->bgvideoram, 1, m->scrollx,
                 m->flipscreen ? ((256 - m->scrolly) & 0xff) : m->scrolly);
    draw_tilemap(m, m->fgvideoram, 0, 0, 0);
    draw_sprites(m);
    rotate_rot270(m);

    SDL_UpdateTexture(m->texture, NULL, m->framebuffer,
                      MRDO_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(m->renderer);
    SDL_RenderCopy(m->renderer, m->texture, NULL, NULL);
    SDL_RenderPresent(m->renderer);
}

void mrdo_run_frame(MrDo *m)
{
    z80_step_n(&m->cpu_main, MRDO_CYCLES_PER_FRAME);
    z80_pulse_irq(&m->cpu_main, 0xff);
    ++m->frame_counter;
}

static inline void set_active_low(uint8_t *port, uint8_t mask, bool pressed)
{
    if (pressed)
        *port &= (uint8_t)~mask;
    else
        *port |= mask;
}

static void set_in0_4way(MrDo *m, uint8_t mask, bool pressed)
{
    if (pressed) {
        m->in0 |= 0x0f;
        m->in0 &= (uint8_t)~mask;
    } else {
        m->in0 |= mask;
    }
}

void mrdo_handle_key(MrDo *m, SDL_Scancode scancode, bool pressed)
{
    /*
     * IN0 real: 0x01 LEFT, 0x02 DOWN, 0x04 RIGHT, 0x08 UP.
     * Como la presentación es ROT270, se traduce desde la flecha visible.
     */
    switch (scancode) {
    case SDL_SCANCODE_UP:  set_in0_4way(m, 0x08, pressed); break;
    case SDL_SCANCODE_DOWN: set_in0_4way(m, 0x02, pressed); break;
    case SDL_SCANCODE_RIGHT:    set_in0_4way(m, 0x04, pressed); break;
    case SDL_SCANCODE_LEFT:  set_in0_4way(m, 0x01, pressed); break;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_SPACE: set_active_low(&m->in0, 0x10, pressed); break;
    case SDL_SCANCODE_1:     set_active_low(&m->in0, 0x20, pressed); break;
    case SDL_SCANCODE_2:     set_active_low(&m->in0, 0x40, pressed); break;
    case SDL_SCANCODE_T:     set_active_low(&m->in0, 0x80, pressed); break;
    case SDL_SCANCODE_5:     set_active_low(&m->in1, 0x40, pressed); break;
    case SDL_SCANCODE_6:     set_active_low(&m->in1, 0x80, pressed); break;
    case SDL_SCANCODE_F1:    set_active_low(&m->dsw1, 0x04, pressed); break;
    case SDL_SCANCODE_F2:    if (pressed) m->turbo = !m->turbo; break;
    case SDL_SCANCODE_ESCAPE: if (pressed) m->quit = true; break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    const char *romdir = argc > 1 ? argv[1] : "roms/mrdo";
    static MrDo m;

    if (mrdo_init(&m) != 0) {
        mrdo_destroy(&m);
        return EXIT_FAILURE;
    }

    if (mrdo_load_romset(&m, romdir) != 0) {
        fprintf(stderr, "ROM set incompleto en %s\n", romdir);
        mrdo_destroy(&m);
        return EXIT_FAILURE;
    }

    const double frame_ms = 1000.0 / 60.0;
    while (!m.quit) {
        uint32_t start = SDL_GetTicks();
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                m.quit = true;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                mrdo_handle_key(&m, event.key.keysym.scancode, true);
            } else if (event.type == SDL_KEYUP) {
                mrdo_handle_key(&m, event.key.keysym.scancode, false);
            }
        }

        mrdo_run_frame(&m);
        if (!m.turbo || ((m.frame_counter & 7U) == 0))
            mrdo_render(&m);

        if (!m.turbo) {
            uint32_t elapsed = SDL_GetTicks() - start;
            if ((double)elapsed < frame_ms)
                SDL_Delay((uint32_t)(frame_ms - (double)elapsed));
        }
    }

    mrdo_destroy(&m);
    return EXIT_SUCCESS;
}
