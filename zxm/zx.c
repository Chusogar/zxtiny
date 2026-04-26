#include "zx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ZXSpectrum spec;

// Paleta de colores estándar del ZX Spectrum (ARGB8888)
static const uint32_t palette[16] = {
    0xFF000000, 0xFF0000D7, 0xFFD70000, 0xFFD700D7, 0xFF00D700, 0xFF00D7D7, 0xFFD7D700, 0xFFD7D7D7,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
};

// =============================================================================
// Callbacks de Memoria e I/O para JGZ80
// =============================================================================

static uint8_t mem_read(void* userdata, uint16_t addr) {
    ZXSpectrum* s = (ZXSpectrum*)userdata;
    return s->memory[addr];
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    ZXSpectrum* s = (ZXSpectrum*)userdata;
    if (addr >= 0x4000)
        s->memory[addr] = val;
}

static uint8_t port_in(z80* z, uint16_t port) {
    (void)z;
    // Puerto de la ULA: bits bajos a 0
    if ((port & 0x01) == 0) {
        uint8_t result = 0xFF;
        for (int i = 0; i < 8; i++) {
            if ((port & (1 << (i + 8))) == 0)
                result &= keyboard_matrix[i];
        }
        // Bit 6 = EAR de entrada (cinta).
        // La ROM lee el bit 6 del puerto 0xFE para detectar pulsos de cinta.
        // mic_bit se actualiza cada T-state por tap_update().
        if (mic_bit)
            result |=  0x40;
        else
            result &= ~0x40;
        return result;
    }
    return 0xFF;
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z;
    if ((port & 0x01) == 0) {
        border_color = val & 0x07;
        ear_bit      = (val & 0x10) >> 4;
    }
}

// =============================================================================
// Reproductor de cinta TAP por pulsos
// =============================================================================

// Carga el fichero TAP completo en memoria y arranca la reproducción.
int spectrum_load_tap(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    free(s->tap.data);
    s->tap.data = (uint8_t*)malloc((size_t)sz);
    if (!s->tap.data) { fclose(f); return -1; }
    fread(s->tap.data, 1, (size_t)sz, f);
    fclose(f);

    s->tap.size   = (uint32_t)sz;
    s->tap.pos    = 0;
    s->tap.active = false;   // Espera a que la ROM pida datos (LOAD)
    s->tap.state  = TAP_STATE_IDLE;
    s->tap.ear    = 0;
    mic_bit       = 0;

    printf("[TAP] Fichero cargado: %s (%ld bytes)\n", filename, sz);
    return 0;
}

// Inicia la reproducción del siguiente bloque TAP.
// Un bloque TAP tiene: 2 bytes de longitud (little-endian) + N bytes de datos.
static bool tap_next_block(TAPPlayer* t) {
    if (!t->data || t->pos + 2 > t->size) {
        t->state  = TAP_STATE_IDLE;
        t->active = false;
        printf("[TAP] Fin de cinta.\n");
        return false;
    }

    t->block_len = (uint32_t)t->data[t->pos] | ((uint32_t)t->data[t->pos + 1] << 8);
    t->pos += 2;

    if (t->pos + t->block_len > t->size) {
        t->state  = TAP_STATE_IDLE;
        t->active = false;
        printf("[TAP] Bloque truncado. Fin de cinta.\n");
        return false;
    }

    t->byte_pos = 0;
    t->bit_mask = 0x80;

    // El primer byte del bloque indica el tipo: 0x00 = cabecera, otro = datos
    uint8_t flag = t->data[t->pos];
    t->pilot_count = (flag == 0x00) ? TAP_PILOT_HEADER : TAP_PILOT_DATA;

    t->state       = TAP_STATE_PILOT;
    t->pulse_cycles = TAP_PILOT_PULSE;
    t->ear         = 1;   // Arranca con nivel alto

    printf("[TAP] Bloque %u bytes, tipo %s\n",
           t->block_len, (flag == 0x00) ? "cabecera" : "datos");
    return true;
}

// Avanza el reproductor TAP tantos ciclos como se le indican.
// Actualiza mic_bit con el nivel EAR actual.
// Debe llamarse una vez por cada instrucción Z80 ejecutada.
static void tap_update(TAPPlayer* t, int cycles) {
    if (!t->active || t->state == TAP_STATE_IDLE) return;

    t->pulse_cycles -= cycles;

    // Mientras el pulso actual haya terminado, avanzar la máquina de estados
    while (t->pulse_cycles <= 0) {
        // Invertir el nivel EAR al terminar cada semi-pulso
        t->ear ^= 1;

        switch (t->state) {

        case TAP_STATE_PILOT:
            t->pilot_count--;
            if (t->pilot_count <= 0) {
                // Pasar a sync1
                t->state        = TAP_STATE_SYNC1;
                t->pulse_cycles += TAP_SYNC1_PULSE;
            } else {
                t->pulse_cycles += TAP_PILOT_PULSE;
            }
            break;

        case TAP_STATE_SYNC1:
            t->state        = TAP_STATE_SYNC2;
            t->pulse_cycles += TAP_SYNC2_PULSE;
            break;

        case TAP_STATE_SYNC2:
            // Empezar datos
            t->state    = TAP_STATE_DATA;
            t->byte_pos = 0;
            t->bit_mask = 0x80;
            {
                uint8_t cur_byte = t->data[t->pos + t->byte_pos];
                int pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
                t->pulse_cycles += pw;
            }
            break;

        case TAP_STATE_DATA: {
            // Cada bit son 2 semi-pulsos de igual duración.
            // La inversión de ear ya ocurrió arriba; hay que saber si
            // estamos en la primera o segunda mitad del bit.
            // Usamos bit_mask para rastrear avance: avanzamos bit cuando ear=0
            // (fin del segundo semi-pulso).
            uint8_t cur_byte = t->data[t->pos + t->byte_pos];
            int pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;

            if (t->ear == 0) {
                // Segunda mitad terminó: avanzar al siguiente bit
                t->bit_mask >>= 1;
                if (t->bit_mask == 0) {
                    t->bit_mask = 0x80;
                    t->byte_pos++;
                    if (t->byte_pos >= t->block_len) {
                        // Bloque terminado: pausa y siguiente bloque
                        t->pos += t->block_len;
                        t->state        = TAP_STATE_PAUSE;
                        t->pulse_cycles += TAP_PAUSE_CYCLES;
                        break;
                    }
                    cur_byte = t->data[t->pos + t->byte_pos];
                }
                pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
            }
            t->pulse_cycles += pw;
            break;
        }

        case TAP_STATE_PAUSE:
            // Pausa terminada: intentar siguiente bloque
            t->ear = 0;
            if (!tap_next_block(t)) return;
            break;

        default:
            return;
        }
    }

    mic_bit = t->ear;
}

// Arranca la reproducción (llamar cuando la ROM ejecuta LD-BYTES / LOAD).
// En un emulador real se podría detectar automáticamente; aquí se inicia
// manualmente al pulsar F1 o al cargar el fichero.
static void tap_start(TAPPlayer* t) {
    if (!t->data || t->size == 0) return;
    t->pos    = 0;   // Rebobinar
    t->active = true;
    printf("[TAP] Reproducción iniciada.\n");
    tap_next_block(t);
}

// =============================================================================
// Inicialización y destrucción
// =============================================================================

void spectrum_init(ZXSpectrum* s) {
    memset(s, 0, sizeof(ZXSpectrum));
    for (int i = 0; i < 8; i++) keyboard_matrix[i] = 0xFF;

    z80_init(&s->cpu);
    s->cpu.userdata  = s;
    s->cpu.read_byte = mem_read;
    s->cpu.write_byte= mem_write;
    s->cpu.port_in   = port_in;
    s->cpu.port_out  = port_out;
    z80_reset(&s->cpu);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    s->window   = SDL_CreateWindow("ZX Spectrum 48K Emulador",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   640, 480, SDL_WINDOW_SHOWN);
    s->renderer = SDL_CreateRenderer(s->window, -1,
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    s->texture  = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, 256, 192);

    SDL_AudioSpec wanted, have;
    SDL_zero(wanted);
    wanted.freq     = 44100;
    wanted.format   = AUDIO_F32;
    wanted.channels = 1;
    wanted.samples  = 1024;
    s->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &have, 0);
    if (s->audio_dev > 0) SDL_PauseAudioDevice(s->audio_dev, 0);
}

void spectrum_destroy(ZXSpectrum* s) {
    free(s->tap.data);
    s->tap.data = NULL;
    SDL_DestroyTexture(s->texture);
    SDL_DestroyRenderer(s->renderer);
    SDL_DestroyWindow(s->window);
    SDL_CloseAudioDevice(s->audio_dev);
    SDL_Quit();
}

// =============================================================================
// Carga de ROM y SNA
// =============================================================================

int spectrum_load_rom(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    fread(s->memory, 1, 16384, f);
    fclose(f);
    return 0;
}

int spectrum_load_sna(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;

    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) { fclose(f); return -1; }
    if (fread(s->memory + 0x4000, 1, 49152, f) != 49152) { fclose(f); return -1; }
    fclose(f);

    s->cpu.i   = header[0];
    s->cpu.l_  = header[1]; s->cpu.h_ = header[2];
    s->cpu.e_  = header[3]; s->cpu.d_ = header[4];
    s->cpu.c_  = header[5]; s->cpu.b_ = header[6];
    s->cpu.f_  = header[7]; s->cpu.a_ = header[8];
    s->cpu.l   = header[9]; s->cpu.h  = header[10];
    s->cpu.e   = header[11]; s->cpu.d = header[12];
    s->cpu.c   = header[13]; s->cpu.b = header[14];
    s->cpu.iy  = header[15] | (header[16] << 8);
    s->cpu.ix  = header[17] | (header[18] << 8);
    uint8_t iff2 = (header[19] & 0x04) != 0;
    s->cpu.iff2 = iff2; s->cpu.iff1 = iff2;
    s->cpu.r   = header[20];
    s->cpu.f   = header[21]; s->cpu.a = header[22];
    s->cpu.sp  = header[23] | (header[24] << 8);
    s->cpu.interrupt_mode = header[25];
    border_color = header[26];

    uint16_t sp = s->cpu.sp;
    s->cpu.pc = s->memory[sp] | (s->memory[sp + 1] << 8);
    s->cpu.sp += 2;

    return 0;
}

// =============================================================================
// Teclado
// =============================================================================

void spectrum_handle_key(ZXSpectrum* s, SDL_Scancode key, bool pressed) {
    uint8_t row = 255, bit = 255;
    switch (key) {
        case SDL_SCANCODE_LSHIFT: row=0; bit=0; break;
        case SDL_SCANCODE_Z:      row=0; bit=1; break;
        case SDL_SCANCODE_X:      row=0; bit=2; break;
        case SDL_SCANCODE_C:      row=0; bit=3; break;
        case SDL_SCANCODE_V:      row=0; bit=4; break;
        case SDL_SCANCODE_A:      row=1; bit=0; break;
        case SDL_SCANCODE_S:      row=1; bit=1; break;
        case SDL_SCANCODE_D:      row=1; bit=2; break;
        case SDL_SCANCODE_F:      row=1; bit=3; break;
        case SDL_SCANCODE_G:      row=1; bit=4; break;
        case SDL_SCANCODE_Q:      row=2; bit=0; break;
        case SDL_SCANCODE_W:      row=2; bit=1; break;
        case SDL_SCANCODE_E:      row=2; bit=2; break;
        case SDL_SCANCODE_R:      row=2; bit=3; break;
        case SDL_SCANCODE_T:      row=2; bit=4; break;
        case SDL_SCANCODE_1:      row=3; bit=0; break;
        case SDL_SCANCODE_2:      row=3; bit=1; break;
        case SDL_SCANCODE_3:      row=3; bit=2; break;
        case SDL_SCANCODE_4:      row=3; bit=3; break;
        case SDL_SCANCODE_5:      row=3; bit=4; break;
        case SDL_SCANCODE_0:      row=4; bit=0; break;
        case SDL_SCANCODE_9:      row=4; bit=1; break;
        case SDL_SCANCODE_8:      row=4; bit=2; break;
        case SDL_SCANCODE_7:      row=4; bit=3; break;
        case SDL_SCANCODE_6:      row=4; bit=4; break;
        case SDL_SCANCODE_P:      row=5; bit=0; break;
        case SDL_SCANCODE_O:      row=5; bit=1; break;
        case SDL_SCANCODE_I:      row=5; bit=2; break;
        case SDL_SCANCODE_U:      row=5; bit=3; break;
        case SDL_SCANCODE_Y:      row=5; bit=4; break;
        case SDL_SCANCODE_RETURN: row=6; bit=0; break;
        case SDL_SCANCODE_L:      row=6; bit=1; break;
        case SDL_SCANCODE_K:      row=6; bit=2; break;
        case SDL_SCANCODE_J:      row=6; bit=3; break;
        case SDL_SCANCODE_H:      row=6; bit=4; break;
        case SDL_SCANCODE_SPACE:  row=7; bit=0; break;
        case SDL_SCANCODE_RSHIFT: row=7; bit=1; break;
        case SDL_SCANCODE_M:      row=7; bit=2; break;
        case SDL_SCANCODE_N:      row=7; bit=3; break;
        case SDL_SCANCODE_B:      row=7; bit=4; break;

        // F1: iniciar/rebobinar reproducción TAP
        case SDL_SCANCODE_F1:
            if (pressed) tap_start(&s->tap);
            return;

        default: break;
    }
    if (row != 255) {
        if (pressed) keyboard_matrix[row] &= ~(1 << bit);
        else         keyboard_matrix[row] |=  (1 << bit);
    }
}

// =============================================================================
// Bucle principal del frame
// =============================================================================

void spectrum_run_frame(ZXSpectrum* s) {
    const int t_states_per_frame  = 69888;
    const int t_states_per_sample = t_states_per_frame / 882;
    int cycles_this_frame = 0;
    int next_sample_cycle = t_states_per_sample;

    z80_pulse_irq(&s->cpu, 0xFF);

    while (cycles_this_frame < t_states_per_frame) {
        int cycles = (int)z80_step(&s->cpu);
        cycles_this_frame += cycles;

        // Avanzar el reproductor TAP sincronizadamente con el Z80
        tap_update(&s->tap, cycles);

        // Muestrear beeper (mezcla EAR de salida + EAR de cinta para audio)
        while (cycles_this_frame >= next_sample_cycle) {
            if (s->audio_pos < 882) {
                // El beeper usa ear_bit (puerto de escritura).
                // La cinta también emite sonido: mezclamos mic_bit.
                float level = 0.0f;
                if (ear_bit)  level += 0.15f;
                if (mic_bit)  level += 0.15f;
                if (level == 0.0f) level = -0.2f;
                s->audio_buffer[s->audio_pos++] = level;
            }
            next_sample_cycle += t_states_per_sample;
        }
    }

    if (s->audio_dev > 0 && s->audio_pos > 0) {
        SDL_QueueAudio(s->audio_dev, s->audio_buffer, s->audio_pos * sizeof(float));
        s->audio_pos = 0;
    }
    s->frame_counter++;
}

// =============================================================================
// Renderizado
// =============================================================================

void spectrum_render(ZXSpectrum* s) {
    bool flash_phase = (s->frame_counter & 16) != 0;

    for (int y = 0; y < 192; y++) {
        int vram_y = (y & 0xC0) | ((y & 0x07) << 3) | ((y & 0x38) >> 3);
        for (int x = 0; x < 32; x++) {
            uint8_t pixels = s->memory[0x4000 + vram_y * 32 + x];
            uint8_t attr   = s->memory[0x5800 + (y / 8) * 32 + x];
            bool bright    = (attr & 0x40) != 0;
            bool flash     = (attr & 0x80) != 0;
            int ink   = (attr & 0x07) | (bright ? 8 : 0);
            int paper = ((attr & 0x38) >> 3) | (bright ? 8 : 0);
            if (flash && flash_phase) { int t = ink; ink = paper; paper = t; }
            for (int b = 0; b < 8; b++) {
                bool is_ink = (pixels & (0x80 >> b)) != 0;
                s->screen_buffer[y * 256 + (x * 8 + b)] = palette[is_ink ? ink : paper];
            }
        }
    }

    SDL_UpdateTexture(s->texture, NULL, s->screen_buffer, 256 * sizeof(uint32_t));

    uint32_t border = palette[border_color];
    SDL_SetRenderDrawColor(s->renderer,
                           (border >> 16) & 0xFF, (border >> 8) & 0xFF, border & 0xFF, 255);
    SDL_RenderClear(s->renderer);
    SDL_Rect dst_rect = { 64, 48, 512, 384 };
    SDL_RenderCopy(s->renderer, s->texture, NULL, &dst_rect);
    SDL_RenderPresent(s->renderer);
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    spectrum_init(&spec);

    if (spectrum_load_rom(&spec, "48.rom") != 0)
        printf("Aviso: '48.rom' no encontrada.\n");

    if (argc > 1) {
        const char* file = argv[1];
        size_t len = strlen(file);

        // Detectar extensión
        if (len > 4 && (strcmp(file + len - 4, ".tap") == 0 ||
                        strcmp(file + len - 4, ".TAP") == 0)) {
            if (spectrum_load_tap(&spec, file) == 0) {
                printf("TAP cargado: %s\n", file);
                printf("Pulsa F1 para iniciar la reproducción de la cinta.\n");
                printf("Escribe en el Spectrum: LOAD \"\" y pulsa ENTER, luego F1.\n");
            } else {
                printf("Error leyendo TAP: %s\n", file);
            }
        } else if (len > 4 && (strcmp(file + len - 4, ".sna") == 0 ||
                               strcmp(file + len - 4, ".SNA") == 0)) {
            if (spectrum_load_sna(&spec, file) == 0)
                printf("Snapshot cargado: %s\n", file);
            else
                printf("Error leyendo SNA: %s\n", file);
        } else {
            printf("Formato no reconocido. Se admiten: .sna, .tap\n");
        }
    } else {
        printf("Uso: %s [archivo.sna | archivo.tap]\n", argv[0]);
        printf("Con .tap: escribe LOAD \"\" en el Spectrum y pulsa F1.\n");
    }

    uint32_t frame_ticks = 1000 / 50;

    while (!spec.quit) {
        uint32_t start_time = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                spec.quit = true;
            else if (e.type == SDL_KEYDOWN)
                spectrum_handle_key(&spec, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)
                spectrum_handle_key(&spec, e.key.keysym.scancode, false);
        }

        spectrum_run_frame(&spec);
        spectrum_render(&spec);

        uint32_t elapsed = SDL_GetTicks() - start_time;
        if (elapsed < frame_ticks)
            SDL_Delay(frame_ticks - elapsed);
    }

    spectrum_destroy(&spec);
    return 0;
}
