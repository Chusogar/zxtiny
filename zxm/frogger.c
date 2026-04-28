#include "frogger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Frogger frog;

// =============================================================================
// AY-3-8910 (emulacion simplificada)
// =============================================================================

static void ay_reset(AY8910* ay) {
    memset(ay, 0, sizeof(*ay));
    ay->noise_shift = 1;
}

static void ay_write_reg(AY8910* ay, uint8_t reg, uint8_t val) {
    if (reg >= AY_NUM_REGS) return;
    ay->regs[reg] = val;
}

static uint8_t ay_read_reg(AY8910* ay) {
    if (ay->latch >= AY_NUM_REGS) return 0xFF;
    return ay->regs[ay->latch];
}

static float ay_generate_sample(AY8910* ay) {
    static const float vol_table[16] = {
        0.0f, 0.0078f, 0.0110f, 0.0156f, 0.0220f, 0.0311f, 0.0440f, 0.0622f,
        0.0880f, 0.1245f, 0.1760f, 0.2489f, 0.3520f, 0.4978f, 0.7040f, 1.0f
    };

    float output = 0.0f;

    // Tone periods
    for (int ch = 0; ch < 3; ch++) {
        uint16_t period = (uint16_t)(ay->regs[ch * 2] | ((ay->regs[ch * 2 + 1] & 0x0F) << 8));
        if (period == 0) period = 1;
        ay->tone_count[ch]++;
        if (ay->tone_count[ch] >= period) {
            ay->tone_count[ch] = 0;
            ay->tone_output[ch] ^= 1;
        }

        uint8_t mixer = ay->regs[7];
        bool tone_en  = !((mixer >> ch) & 1);
        bool noise_en = !((mixer >> (ch + 3)) & 1);

        bool tone_out  = tone_en ? ay->tone_output[ch] : true;
        bool noise_out = noise_en ? ay->noise_output : true;

        if (tone_out && noise_out) {
            uint8_t vol_reg = ay->regs[8 + ch];
            int vol = (vol_reg & 0x10) ? 15 : (vol_reg & 0x0F);
            output += vol_table[vol];
        }
    }

    // Noise
    uint16_t noise_period = ay->regs[6] & 0x1F;
    if (noise_period == 0) noise_period = 1;
    ay->noise_count++;
    if (ay->noise_count >= noise_period) {
        ay->noise_count = 0;
        if (ay->noise_shift & 1)
            ay->noise_shift ^= 0x24000;
        ay->noise_shift >>= 1;
        ay->noise_output = ay->noise_shift & 1;
    }

    return output / 3.0f;
}

// =============================================================================
// Decodificacion de graficos (tiles 8x8 y sprites 16x16)
//
// Formato Galaxian/Frogger: 2 bitplanes en ROMs separadas (0x0000 y 0x0800)
// Cada tile: 8 bytes por plano = 16 bytes total
// Monitor vertical: los graficos se almacenan rotados 90 grados
// =============================================================================

static void decode_tiles(Frogger* f) {
    for (int t = 0; t < 256; t++) {
        for (int row = 0; row < 8; row++) {
            uint8_t b0 = f->gfx_rom[(t * 8 + row)];
            uint8_t b1 = f->gfx_rom[0x0800 + (t * 8 + row)];
            for (int col = 0; col < 8; col++) {
                int bit = 7 - col;
                uint8_t pixel = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
                f->tiles[t][row][col] = pixel;
            }
        }
    }
}

static void decode_sprites(Frogger* f) {
    for (int s = 0; s < 64; s++) {
        for (int row = 0; row < 16; row++) {
            int sub_row = row & 7;
            for (int col = 0; col < 16; col++) {
                int sub_tile = col / 8;
                int sub_col = col & 7;
                int bit = 7 - sub_col;
                int tile_idx = (s * 4) + (row / 8) * 2 + sub_tile;
                uint8_t rb0 = f->gfx_rom[(tile_idx * 8 + sub_row) & 0x07FF];
                uint8_t rb1 = f->gfx_rom[0x0800 + ((tile_idx * 8 + sub_row) & 0x07FF)];
                uint8_t pixel = ((rb0 >> bit) & 1) | (((rb1 >> bit) & 1) << 1);
                f->sprites[s][row][col] = pixel;
            }
        }
    }
}

// =============================================================================
// Paleta de colores (PROM pr-91.6l, 32 bytes)
//
// Formato por byte:  bits 0-2 = R (ponderados), bits 3-5 = G, bits 6-7 = B
// Pesos: R = bit0*33 + bit1*71 + bit2*151
//        G = bit3*33 + bit4*71 + bit5*151
//        B = bit6*78 + bit7*178
// =============================================================================

static void decode_palette(Frogger* f) {
    for (int i = 0; i < 32; i++) {
        uint8_t val = f->color_prom[i];
        int r = ((val >> 0) & 1) * 33 + ((val >> 1) & 1) * 71 + ((val >> 2) & 1) * 151;
        int g = ((val >> 3) & 1) * 33 + ((val >> 4) & 1) * 71 + ((val >> 5) & 1) * 151;
        int b = ((val >> 6) & 1) * 78 + ((val >> 7) & 1) * 178;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        f->palette[i] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// =============================================================================
// Callbacks de memoria - CPU principal
// =============================================================================

static uint8_t main_mem_read(void* userdata, uint16_t addr) {
    Frogger* f = (Frogger*)userdata;

    if (addr < FROGGER_ROM_SIZE)
        return f->main_rom[addr];

    if (addr >= FROGGER_RAM_START && addr < FROGGER_RAM_START + FROGGER_RAM_SIZE)
        return f->main_ram[addr - FROGGER_RAM_START];

    if (addr == 0x8800)
        return 0xFF; // watchdog read

    if (addr >= FROGGER_VRAM_START && addr < FROGGER_VRAM_START + FROGGER_VRAM_SIZE)
        return f->video_ram[addr - FROGGER_VRAM_START];

    // A800-AFFF mirrors A800-ABFF
    if (addr >= 0xAC00 && addr < 0xB000)
        return f->video_ram[addr - 0xAC00];

    if (addr >= FROGGER_OBJRAM_START && addr < FROGGER_OBJRAM_START + FROGGER_OBJRAM_SIZE)
        return f->obj_ram[addr - FROGGER_OBJRAM_START];

    // B000-B7FF mirrors B000-B0FF
    if (addr >= 0xB100 && addr < 0xB800)
        return f->obj_ram[(addr - FROGGER_OBJRAM_START) & 0xFF];

    // PPI 8255 reads (I/O ports E000-E006)
    // Frogger maps PPI at C000-FFFF, but uses address bit swapping
    // Address lines D7-D0 are used as: addr bits 1,0 select PPI port
    if (addr >= 0xC000) {
        // Frogger swaps address lines: A0 <-> A1 for PPI access
        uint16_t ppi_addr = ((addr >> 1) & 1) | (((addr >> 0) & 1) << 1);
        switch (ppi_addr & 0x07) {
        case 0: return f->input[0]; // IN0
        case 1: return f->input[1]; // IN1
        case 2: return f->input[2]; // IN2
        default: return 0xFF;
        }
    }

    return 0xFF;
}

static void main_mem_write(void* userdata, uint16_t addr, uint8_t val) {
    Frogger* f = (Frogger*)userdata;

    if (addr < FROGGER_ROM_SIZE)
        return; // ROM

    if (addr >= FROGGER_RAM_START && addr < FROGGER_RAM_START + FROGGER_RAM_SIZE) {
        f->main_ram[addr - FROGGER_RAM_START] = val;
        return;
    }

    if (addr >= FROGGER_VRAM_START && addr < FROGGER_VRAM_START + FROGGER_VRAM_SIZE) {
        f->video_ram[addr - FROGGER_VRAM_START] = val;
        return;
    }

    if (addr >= 0xAC00 && addr < 0xB000) {
        f->video_ram[addr - 0xAC00] = val;
        return;
    }

    if (addr >= FROGGER_OBJRAM_START && addr < FROGGER_OBJRAM_START + FROGGER_OBJRAM_SIZE) {
        f->obj_ram[addr - FROGGER_OBJRAM_START] = val;
        // Scroll por fila se almacena en obj_ram[0x00-0x1F]
        if ((addr & 0xFF) < 0x20)
            f->row_scroll[addr & 0x1F] = val;
        return;
    }

    // Registros de control
    if (addr == 0xB808 || (addr >= 0xB808 && (addr & 0xF81C) == 0xB808)) {
        f->irq_enable = (val & 1) != 0;
        return;
    }
    if ((addr & 0xF81C) == 0xB80C) {
        f->flip_y = (val & 1) != 0;
        return;
    }
    if ((addr & 0xF81C) == 0xB810) {
        f->flip_x = (val & 1) != 0;
        return;
    }

    // PPI writes (sound command / control)
    if (addr >= 0xC000) {
        uint16_t ppi_addr = ((addr >> 1) & 1) | (((addr >> 0) & 1) << 1);
        switch (ppi_addr & 0x07) {
        case 0: // Sound command
            f->sound_cmd = val;
            break;
        case 1: // Sound control (bit 0 = trigger IRQ to sound CPU)
            if (val & 1)
                f->sound_irq = true;
            break;
        }
        return;
    }
}

// No I/O ports used by main CPU (everything memory-mapped)
static uint8_t main_port_in(z80* z, uint16_t port) {
    (void)z; (void)port;
    return 0xFF;
}

static void main_port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z; (void)port; (void)val;
}

// =============================================================================
// Callbacks de memoria - CPU de sonido
// =============================================================================

static uint8_t sound_mem_read(void* userdata, uint16_t addr) {
    Frogger* f = (Frogger*)userdata;

    if (addr < FROGGER_SNDROM_SIZE)
        return f->sound_rom[addr];

    if (addr >= FROGGER_SNDRAM_START && addr < FROGGER_SNDRAM_START + FROGGER_SNDRAM_SIZE)
        return f->sound_ram[addr - FROGGER_SNDRAM_START];

    return 0xFF;
}

static void sound_mem_write(void* userdata, uint16_t addr, uint8_t val) {
    Frogger* f = (Frogger*)userdata;

    if (addr >= FROGGER_SNDRAM_START && addr < FROGGER_SNDRAM_START + FROGGER_SNDRAM_SIZE)
        f->sound_ram[addr - FROGGER_SNDRAM_START] = val;
}

static uint8_t sound_port_in(z80* z, uint16_t port) {
    Frogger* f = (Frogger*)z->userdata;
    uint8_t p = port & 0xFF;

    if (p == 0x40) {
        // AY read: latch selects which AY and register
        // Frogger uses 2 AY chips. Port A of AY#1 receives the sound command
        int chip = (f->ay[0].latch >= AY_NUM_REGS) ? 1 : 0;
        if (chip == 0 && f->ay[0].latch == 14)
            return f->sound_cmd; // Port A = sound command from main CPU
        return ay_read_reg(&f->ay[chip]);
    }

    return 0xFF;
}

static void sound_port_out(z80* z, uint16_t port, uint8_t val) {
    Frogger* f = (Frogger*)z->userdata;
    uint8_t p = port & 0xFF;

    if (p == 0x80) {
        // AY address latch
        // Frogger maps: bits 0-3 = register, bit 4 = chip select
        int chip = (val >> 4) & 1;
        f->ay[chip].latch = val & 0x0F;
    } else if (p == 0x40) {
        // AY data write
        for (int i = 0; i < 2; i++) {
            if (f->ay[i].latch < AY_NUM_REGS)
                ay_write_reg(&f->ay[i], f->ay[i].latch, val);
        }
    }
}

// =============================================================================
// Renderizado de video
//
// Frogger usa hardware Galaxian: tilemap 32x28 con scroll por fila,
// sobre el cual se dibujan 8 sprites 16x16.
// Monitor vertical: la imagen se rota 90 grados a la izquierda.
//
// Representacion en VRAM (sin rotar):
//   - 32 columnas x 32 filas (solo 28 filas visibles = 224 pixeles)
//   - Cada byte de VRAM = indice de tile
//   - Atributos en obj_ram[0x00-0x3F]: scroll + color
//
// Resultado en framebuffer: 224 x 256 (ya rotado)
// =============================================================================

static void render_tilemap(Frogger* f) {
    for (int row = 0; row < FROGGER_TILE_ROWS; row++) {
        int scroll = f->obj_ram[row * 2] & 0xFF;
        int color_base = (f->obj_ram[row * 2 + 1] & 0x07) * 4;

        for (int col = 0; col < FROGGER_TILE_COLS; col++) {
            // VRAM layout: columna-mayor (cada columna = 32 bytes)
            int vram_addr = col * 32 + row;
            if (vram_addr >= FROGGER_VRAM_SIZE) continue;
            uint8_t tile_idx = f->video_ram[vram_addr];

            for (int py = 0; py < FROGGER_TILE_SIZE; py++) {
                for (int px = 0; px < FROGGER_TILE_SIZE; px++) {
                    uint8_t pixel = f->tiles[tile_idx][py][px];

                    // Posicion en pantalla sin rotar
                    // Rotar 90 grados: la pantalla del arcade es vertical
                    int fx = FROGGER_SCREEN_W - 1 - (col * FROGGER_TILE_SIZE + py);
                    int fy = row * FROGGER_TILE_SIZE + px;

                    // Aplicar scroll en la dimension correcta
                    fy = (fy + scroll) & 0xFF;

                    if (f->flip_x) fx = FROGGER_SCREEN_W - 1 - fx;
                    if (f->flip_y) fy = FROGGER_SCREEN_H - 1 - fy;

                    if (fx >= 0 && fx < FROGGER_SCREEN_W &&
                        fy >= 0 && fy < FROGGER_SCREEN_H) {
                        uint32_t color = f->palette[color_base + pixel];
                        f->framebuffer[fy * FROGGER_SCREEN_W + fx] = color;
                    }
                }
            }
        }
    }
}

static void render_sprites(Frogger* f) {
    // Sprites en obj_ram[0x40-0x5F]: 4 bytes por sprite x 8 sprites
    // Byte 0: Y posicion
    // Byte 1: tile index (bits 1-7) + flip Y (bit 0) - Frogger-specific encoding
    // Byte 2: color (bits 0-2) + flip X (bit 3)
    // Byte 3: X posicion
    for (int i = 0; i < FROGGER_MAX_SPRITES; i++) {
        int base = 0x40 + i * 4;
        uint8_t sy_raw  = f->obj_ram[base + 0];
        uint8_t code    = f->obj_ram[base + 1];
        uint8_t attr    = f->obj_ram[base + 2];
        uint8_t sx_raw  = f->obj_ram[base + 3];

        int sprite_idx = (code >> 2) & 0x3F;
        bool sflip_y = (code >> 1) & 1;
        bool sflip_x = (code >> 0) & 1;
        int color_base = (attr & 0x07) * 4;

        // Posiciones en hardware (sin rotar)
        int sx = sx_raw;
        int sy = 256 - sy_raw - FROGGER_SPRITE_SIZE;

        for (int py = 0; py < FROGGER_SPRITE_SIZE; py++) {
            for (int px = 0; px < FROGGER_SPRITE_SIZE; px++) {
                int tpy = sflip_y ? (FROGGER_SPRITE_SIZE - 1 - py) : py;
                int tpx = sflip_x ? (FROGGER_SPRITE_SIZE - 1 - px) : px;
                uint8_t pixel = f->sprites[sprite_idx][tpy][tpx];

                if (pixel == 0) continue; // transparente

                // Rotar 90 grados para monitor vertical
                int fx = FROGGER_SCREEN_W - 1 - (sy + py);
                int fy = sx + px;

                if (f->flip_x) fx = FROGGER_SCREEN_W - 1 - fx;
                if (f->flip_y) fy = FROGGER_SCREEN_H - 1 - fy;

                if (fx >= 0 && fx < FROGGER_SCREEN_W &&
                    fy >= 0 && fy < FROGGER_SCREEN_H) {
                    uint32_t color = f->palette[color_base + pixel];
                    f->framebuffer[fy * FROGGER_SCREEN_W + fx] = color;
                }
            }
        }
    }
}

// =============================================================================
// Carga de ROMs
//
// Espera los archivos en rom_dir/:
//   frogger.26, frogger.27, frsm3.7   -> main ROM (0000-2FFF, padded to 4000)
//   frogger.608, frogger.609, frogger.610 -> sound ROM (0000-17FF)
//   frogger.607, frogger.606           -> GFX ROM (0000-0FFF)
//   pr-91.6l                           -> color PROM
// =============================================================================

static int load_file(const char* path, uint8_t* dst, size_t offset, size_t max_size) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: no se puede abrir '%s'\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || offset + (size_t)sz > max_size) {
        fprintf(stderr, "Error: tamano incorrecto en '%s'\n", path);
        fclose(fp);
        return -1;
    }
    size_t nread = fread(dst + offset, 1, (size_t)sz, fp);
    (void)nread;
    fclose(fp);
    return 0;
}

int frogger_load_roms(Frogger* f, const char* rom_dir) {
    char path[512];
    int err = 0;

    // Main ROMs
    snprintf(path, sizeof(path), "%s/frogger.26", rom_dir);
    err |= load_file(path, f->main_rom, 0x0000, FROGGER_ROM_SIZE);
    snprintf(path, sizeof(path), "%s/frogger.27", rom_dir);
    err |= load_file(path, f->main_rom, 0x1000, FROGGER_ROM_SIZE);
    snprintf(path, sizeof(path), "%s/frsm3.7", rom_dir);
    err |= load_file(path, f->main_rom, 0x2000, FROGGER_ROM_SIZE);

    // Sound ROMs
    snprintf(path, sizeof(path), "%s/frogger.608", rom_dir);
    err |= load_file(path, f->sound_rom, 0x0000, FROGGER_SNDROM_SIZE);
    snprintf(path, sizeof(path), "%s/frogger.609", rom_dir);
    err |= load_file(path, f->sound_rom, 0x0800, FROGGER_SNDROM_SIZE);
    snprintf(path, sizeof(path), "%s/frogger.610", rom_dir);
    err |= load_file(path, f->sound_rom, 0x1000, FROGGER_SNDROM_SIZE);

    // GFX ROMs
    snprintf(path, sizeof(path), "%s/frogger.607", rom_dir);
    err |= load_file(path, f->gfx_rom, 0x0000, FROGGER_GFX_SIZE);
    snprintf(path, sizeof(path), "%s/frogger.606", rom_dir);
    err |= load_file(path, f->gfx_rom, 0x0800, FROGGER_GFX_SIZE);

    // Color PROM
    snprintf(path, sizeof(path), "%s/pr-91.6l", rom_dir);
    err |= load_file(path, f->color_prom, 0, FROGGER_PROM_SIZE);

    if (err) return -1;

    decode_tiles(f);
    decode_sprites(f);
    decode_palette(f);

    printf("[FROGGER] ROMs cargadas desde '%s'\n", rom_dir);
    return 0;
}

// =============================================================================
// Inicializacion y destruccion
// =============================================================================

void frogger_init(Frogger* f) {
    memset(f, 0, sizeof(*f));

    // Inputs: todos los bits a 1 (invertidos = no pulsado)
    f->input[0] = 0xFF;
    f->input[1] = 0xFF;
    f->input[2] = 0xFF;
    // DIP: 3 vidas, upright
    f->dip_switches = 0x00;

    // CPU principal
    f->main_cpu.userdata   = f;
    f->main_cpu.read_byte  = main_mem_read;
    f->main_cpu.write_byte = main_mem_write;
    f->main_cpu.port_in    = main_port_in;
    f->main_cpu.port_out   = main_port_out;
    z80_reset(&f->main_cpu);

    // CPU de sonido
    f->sound_cpu.userdata  = f;
    f->sound_cpu.read_byte = sound_mem_read;
    f->sound_cpu.write_byte= sound_mem_write;
    f->sound_cpu.port_in   = sound_port_in;
    f->sound_cpu.port_out  = sound_port_out;
    z80_reset(&f->sound_cpu);

    // AY chips
    ay_reset(&f->ay[0]);
    ay_reset(&f->ay[1]);

    // SDL Video
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    f->window = SDL_CreateWindow("Frogger (Konami 1981)",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 FROGGER_SCREEN_W * FROGGER_SCALE,
                                 FROGGER_SCREEN_H * FROGGER_SCALE,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    f->renderer = SDL_CreateRenderer(f->window, -1,
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    f->texture = SDL_CreateTexture(f->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   FROGGER_SCREEN_W, FROGGER_SCREEN_H);
    SDL_RenderSetLogicalSize(f->renderer,
                             FROGGER_SCREEN_W * FROGGER_SCALE,
                             FROGGER_SCREEN_H * FROGGER_SCALE);

    // SDL Audio
    SDL_AudioSpec wanted, have;
    SDL_zero(wanted);
    wanted.freq     = 44100;
    wanted.format   = AUDIO_F32;
    wanted.channels = 1;
    wanted.samples  = 1024;
    f->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &have, 0);
    if (f->audio_dev > 0) SDL_PauseAudioDevice(f->audio_dev, 0);
}

void frogger_destroy(Frogger* f) {
    SDL_DestroyTexture(f->texture);
    SDL_DestroyRenderer(f->renderer);
    SDL_DestroyWindow(f->window);
    SDL_CloseAudioDevice(f->audio_dev);
    SDL_Quit();
    (void)f;
}

// =============================================================================
// Input (teclado a puertos arcade)
//
// IN0: bit7=COIN1, bit6=COIN2, bit5=LEFT1, bit4=RIGHT1, bit3=SHOOT1_1,
//      bit2=CREDIT, bit1=SHOOT1_2, bit0=UP2
// IN1: bit7=START1, bit6=START2, bit5=LEFT2, bit4=RIGHT2, bit3=SHOOT2_1,
//      bit2=SHOOT2_2, bit1:0=LIVES(DIP)
// IN2: bit6=DOWN1, bit4=UP1, bit3=CABINET(DIP), bit0=DOWN2
// Todos los bits invertidos (0=activo)
// =============================================================================

static void frogger_handle_key(Frogger* f, SDL_Scancode key, bool pressed) {
    // Invertir: 0 = pulsado, 1 = no pulsado
    switch (key) {
    // Monedas y creditos
    case SDL_SCANCODE_5:  // COIN 1
        if (pressed) f->input[0] &= ~0x80; else f->input[0] |= 0x80;
        break;
    case SDL_SCANCODE_1:  // START 1
        if (pressed) f->input[1] &= ~0x80; else f->input[1] |= 0x80;
        break;
    case SDL_SCANCODE_2:  // START 2
        if (pressed) f->input[1] &= ~0x40; else f->input[1] |= 0x40;
        break;

    // Jugador 1
    case SDL_SCANCODE_UP:     // UP
        if (pressed) f->input[2] &= ~0x10; else f->input[2] |= 0x10;
        break;
    case SDL_SCANCODE_DOWN:   // DOWN
        if (pressed) f->input[2] &= ~0x40; else f->input[2] |= 0x40;
        break;
    case SDL_SCANCODE_LEFT:   // LEFT
        if (pressed) f->input[0] &= ~0x20; else f->input[0] |= 0x20;
        break;
    case SDL_SCANCODE_RIGHT:  // RIGHT
        if (pressed) f->input[0] &= ~0x10; else f->input[0] |= 0x10;
        break;

    // Turbo mode
    case SDL_SCANCODE_F2:
        if (pressed) {
            f->turbo_mode = !f->turbo_mode;
            printf("[EMU] Velocidad %s\n", f->turbo_mode ? "MAXIMA" : "normal");
        }
        break;

    default: break;
    }
}

// =============================================================================
// Bucle de frame
//
// Ejecuta ambos CPUs en paralelo, genera video y audio.
// El CPU principal recibe NMI al final del VBLANK (si irq_enable).
// El CPU de sonido recibe IRQ cuando la CPU principal escribe sound_cmd.
// =============================================================================

void frogger_run_frame(Frogger* f) {
    const int MAIN_CYCLES  = FROGGER_MAIN_CYCLES_PER_FRAME;
    const int SOUND_CYCLES = FROGGER_SOUND_CYCLES_PER_FRAME;
    const int TSTATES_PER_SAMPLE = MAIN_CYCLES / 882;

    int main_done  = 0;
    int sound_done = 0;
    int next_sample_at = TSTATES_PER_SAMPLE;

    // Ejecutar CPU principal
    while (main_done < MAIN_CYCLES) {
        int cycles = (int)z80_step(&f->main_cpu);
        main_done += cycles;

        // Audio sampling
        while (main_done >= next_sample_at) {
            if (f->audio_pos < 882) {
                float sample = ay_generate_sample(&f->ay[0]) +
                               ay_generate_sample(&f->ay[1]);
                f->audio_buffer[f->audio_pos++] = sample * 0.3f;
            }
            next_sample_at += TSTATES_PER_SAMPLE;
        }
    }

    // Ejecutar CPU de sonido
    while (sound_done < SOUND_CYCLES) {
        // Entregar IRQ pendiente de la CPU principal
        if (f->sound_irq) {
            z80_pulse_irq(&f->sound_cpu, 0xFF);
            f->sound_irq = false;
        }
        int cycles = (int)z80_step(&f->sound_cpu);
        sound_done += cycles;
    }

    // VBLANK NMI al CPU principal
    if (f->irq_enable)
        z80_pulse_nmi(&f->main_cpu);

    // Renderizar video
    memset(f->framebuffer, 0, sizeof(f->framebuffer));
    render_tilemap(f);
    render_sprites(f);

    // Audio output
    if (f->audio_dev > 0 && f->audio_pos > 0) {
        if (!f->turbo_mode)
            SDL_QueueAudio(f->audio_dev, f->audio_buffer, f->audio_pos * sizeof(float));
        f->audio_pos = 0;
    }

    f->frame_counter++;
}

// =============================================================================
// Presentacion SDL
// =============================================================================

void frogger_render(Frogger* f) {
    SDL_UpdateTexture(f->texture, NULL, f->framebuffer,
                      FROGGER_SCREEN_W * sizeof(uint32_t));
    SDL_RenderClear(f->renderer);
    SDL_Rect dst = { 0, 0,
                     FROGGER_SCREEN_W * FROGGER_SCALE,
                     FROGGER_SCREEN_H * FROGGER_SCALE };
    SDL_RenderCopy(f->renderer, f->texture, NULL, &dst);
    SDL_RenderPresent(f->renderer);
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    const char* rom_dir = ".";
    if (argc > 1) rom_dir = argv[1];

    frogger_init(&frog);

    if (frogger_load_roms(&frog, rom_dir) != 0) {
        fprintf(stderr, "Error: no se pudieron cargar las ROMs de Frogger.\n");
        fprintf(stderr, "Uso: %s <directorio_roms>\n", argv[0]);
        fprintf(stderr, "  Archivos necesarios:\n");
        fprintf(stderr, "    frogger.26, frogger.27, frsm3.7      (CPU principal)\n");
        fprintf(stderr, "    frogger.608, frogger.609, frogger.610 (CPU sonido)\n");
        fprintf(stderr, "    frogger.607, frogger.606              (graficos)\n");
        fprintf(stderr, "    pr-91.6l                              (paleta)\n");
        fprintf(stderr, "\nControles:\n");
        fprintf(stderr, "  Flechas     -> movimiento\n");
        fprintf(stderr, "  5           -> insertar moneda\n");
        fprintf(stderr, "  1           -> Start 1P\n");
        fprintf(stderr, "  2           -> Start 2P\n");
        fprintf(stderr, "  F2          -> velocidad maxima/normal\n");
        frogger_destroy(&frog);
        return 1;
    }

    printf("Controles:\n");
    printf("  Flechas     -> movimiento\n");
    printf("  5           -> insertar moneda\n");
    printf("  1           -> Start 1P\n");
    printf("  2           -> Start 2P\n");
    printf("  F2          -> velocidad maxima/normal\n");

    const uint32_t FRAME_MS = 1000 / FROGGER_FPS;

    while (!frog.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                frog.quit = true;
            else if (e.type == SDL_KEYDOWN)
                frogger_handle_key(&frog, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)
                frogger_handle_key(&frog, e.key.keysym.scancode, false);
        }

        frogger_run_frame(&frog);

        if (frog.turbo_mode) {
            if ((frog.frame_counter & 7) == 0)
                frogger_render(&frog);
        } else {
            frogger_render(&frog);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS)
                SDL_Delay(FRAME_MS - elapsed);
        }
    }

    frogger_destroy(&frog);
    return 0;
}
