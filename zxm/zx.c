#include "zx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ZXSpectrum spec;

// Paleta de colores estándar del ZX Spectrum (ARGB8888)
static const uint32_t palette[16] = {
    0xFF000000, 0xFF0000D7, 0xFFD70000, 0xFFD700D7,
    0xFF00D700, 0xFF00D7D7, 0xFFD7D700, 0xFFD7D7D7,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
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
    if ((port & 0x01) == 0) {
        uint8_t result = 0xFF;
        for (int i = 0; i < 8; i++)
            if ((port & (1 << (i + 8))) == 0)
                result &= keyboard_matrix[i];
        // Bit 6 = EAR de entrada (cinta).  mic_bit se actualiza ciclo a ciclo
        // en spectrum_run_frame() por tap_update() / tzx_update().
        if (mic_bit) result |=  0x40;
        else         result &= ~0x40;
        return result;
    }
    return 0xFF;
}

// port_out no actualiza border_color directamente; lo hace spectrum_run_frame
// a través de la bandera ula_out_dirty, que permite registrar el cambio en la
// línea ULA exacta en que ocurrió (necesario para efectos de borde por línea).
static uint8_t ula_pending_out = 0;
static bool    ula_out_dirty   = false;

static void port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z;
    if ((port & 0x01) == 0) {
        ula_pending_out = val;
        ula_out_dirty   = true;
    }
}

// =============================================================================
// Reproductor TAP por pulsos
// =============================================================================

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
    s->tap.active = false;
    s->tap.state  = TAP_STATE_IDLE;
    s->tap.ear    = 0;
    s->tape_src   = TAPE_TAP;
    mic_bit       = 0;
    printf("[TAP] Fichero cargado: %s (%ld bytes)\n", filename, sz);
    return 0;
}

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
        printf("[TAP] Bloque truncado.\n");
        return false;
    }
    t->byte_pos    = 0;
    t->bit_mask    = 0x80;
    uint8_t flag   = t->data[t->pos];
    t->pilot_count = (flag == 0x00) ? TAP_PILOT_HEADER : TAP_PILOT_DATA;
    t->state       = TAP_STATE_PILOT;
    t->pulse_cycles= TAP_PILOT_PULSE;
    t->ear         = 1;
    printf("[TAP] Bloque %u bytes, tipo %s\n", t->block_len,
           (flag == 0x00) ? "cabecera" : "datos");
    return true;
}

static void tap_update(TAPPlayer* t, int cycles) {
    if (!t->active || t->state == TAP_STATE_IDLE) return;
    t->pulse_cycles -= cycles;
    while (t->pulse_cycles <= 0) {
        t->ear ^= 1;
        switch (t->state) {
        case TAP_STATE_PILOT:
            t->pilot_count--;
            if (t->pilot_count <= 0) {
                t->state         = TAP_STATE_SYNC1;
                t->pulse_cycles += TAP_SYNC1_PULSE;
            } else {
                t->pulse_cycles += TAP_PILOT_PULSE;
            }
            break;
        case TAP_STATE_SYNC1:
            t->state         = TAP_STATE_SYNC2;
            t->pulse_cycles += TAP_SYNC2_PULSE;
            break;
        case TAP_STATE_SYNC2:
            t->state        = TAP_STATE_DATA;
            t->byte_pos     = 0;
            t->bit_mask     = 0x80;
            t->pulse_cycles += (t->data[t->pos + t->byte_pos] & t->bit_mask)
                               ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
            break;
        case TAP_STATE_DATA: {
            uint8_t cur_byte = t->data[t->pos + t->byte_pos];
            int pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
            if (t->ear == 0) {
                t->bit_mask >>= 1;
                if (t->bit_mask == 0) {
                    t->bit_mask = 0x80;
                    t->byte_pos++;
                    if (t->byte_pos >= t->block_len) {
                        t->pos         += t->block_len;
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
            t->ear = 0;
            if (!tap_next_block(t)) return;
            break;
        default: return;
        }
    }
    mic_bit = t->ear;
}

static void tap_start(TAPPlayer* t) {
    if (!t->data || t->size == 0) return;
    t->pos    = 0;
    t->active = true;
    printf("[TAP] Reproducción iniciada.\n");
    tap_next_block(t);
}

// =============================================================================
// Carga y arranque de TZX  (implementación delegada en tzx.c)
// =============================================================================

int spectrum_load_tzx(ZXSpectrum* s, const char* filename) {
    // Desactivar cualquier fuente previa
    s->tap.active = false;
    tzx_free(&s->tzx);

    if (tzx_load(&s->tzx, filename) != 0)
        return -1;

    s->tape_src = TAPE_TZX;
    mic_bit     = 0;
    return 0;
}

// Arranca la fuente de cinta activa (llamado con F1)
void spectrum_tape_start(ZXSpectrum* s) {
    switch (s->tape_src) {
    case TAPE_TAP:
        tap_start(&s->tap);
        break;
    case TAPE_TZX:
        tzx_start(&s->tzx);
        break;
    default:
        printf("[TAPE] No hay cinta cargada.\n");
        break;
    }
}

// =============================================================================
// Inicialización y destrucción
// =============================================================================

void spectrum_init(ZXSpectrum* s) {
    memset(s, 0, sizeof(ZXSpectrum));
    for (int i = 0; i < 8; i++) keyboard_matrix[i] = 0xFF;
    s->tape_src = TAPE_NONE;

    z80_init(&s->cpu);
    s->cpu.userdata   = s;
    s->cpu.read_byte  = mem_read;
    s->cpu.write_byte = mem_write;
    s->cpu.port_in    = port_in;
    s->cpu.port_out   = port_out;
    z80_reset(&s->cpu);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    s->window   = SDL_CreateWindow("ZX Spectrum 48K Emulador",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   FULL_W * 2, FULL_H * 2,
                                   SDL_WINDOW_SHOWN);
    s->renderer = SDL_CreateRenderer(s->window, -1,
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    s->texture  = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, FULL_W, FULL_H);
    SDL_RenderSetLogicalSize(s->renderer, FULL_W * 2, FULL_H * 2);

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
    tzx_free(&s->tzx);
    SDL_DestroyTexture(s->texture);
    SDL_DestroyRenderer(s->renderer);
    SDL_DestroyWindow(s->window);
    SDL_CloseAudioDevice(s->audio_dev);
    SDL_Quit();
}

// =============================================================================
// ROM y SNA
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
    if (fread(header, 1, 27, f) != 27)      { fclose(f); return -1; }
    if (fread(s->memory + 0x4000, 1, 49152, f) != 49152) { fclose(f); return -1; }
    fclose(f);
    s->cpu.i  = header[0];
    s->cpu.l_ = header[1];  s->cpu.h_ = header[2];
    s->cpu.e_ = header[3];  s->cpu.d_ = header[4];
    s->cpu.c_ = header[5];  s->cpu.b_ = header[6];
    s->cpu.f_ = header[7];  s->cpu.a_ = header[8];
    s->cpu.l  = header[9];  s->cpu.h  = header[10];
    s->cpu.e  = header[11]; s->cpu.d  = header[12];
    s->cpu.c  = header[13]; s->cpu.b  = header[14];
    s->cpu.iy = header[15] | (header[16] << 8);
    s->cpu.ix = header[17] | (header[18] << 8);
    uint8_t iff2 = (header[19] & 0x04) != 0;
    s->cpu.iff2 = iff2; s->cpu.iff1 = iff2;
    s->cpu.r  = header[20];
    s->cpu.f  = header[21]; s->cpu.a = header[22];
    s->cpu.sp = header[23] | (header[24] << 8);
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

        // F1 → iniciar/rebobinar cinta activa (TAP o TZX)
        case SDL_SCANCODE_F1:
            if (pressed) spectrum_tape_start(s);
            return;

        // F2 → detener cinta
        case SDL_SCANCODE_F2:
            if (pressed) {
                s->tap.active = false;
                s->tzx.active = false;
                mic_bit = 0;
                printf("[TAPE] Cinta detenida.\n");
            }
            return;

        default: break;
    }
    if (row != 255) {
        if (pressed) keyboard_matrix[row] &= ~(1 << bit);
        else         keyboard_matrix[row] |=  (1 << bit);
    }
}

// =============================================================================
// Bucle de frame
// =============================================================================
//
// Geometría del ULA 48K:
//   Frame = 312 líneas × 224 T-states = 69 888 T-states/frame
//
//   ULA  0..15   → retrazado vertical superior (invisible)
//   ULA 16..63   → borde superior visible  (48 líneas)
//   ULA 64..255  → área de imagen           (192 líneas)
//   ULA 256..303 → borde inferior visible   (48 líneas)
//   ULA 304..311 → retrazado vertical inferior
//
// Dentro de cada línea (224 T-states):
//   T   0.. 15  → borde izquierdo visible
//   T  16..143  → imagen (256 píxeles, 2T/píxel)
//   T 144..159  → borde derecho visible
//   T 160..223  → retrazado horizontal (invisible)
//
// border_lines[ula_line] se rellena en el punto exacto del frame en que el
// Z80 escribe en el puerto 0xFE, lo que permite efectos de borde por línea.
// =============================================================================

void spectrum_run_frame(ZXSpectrum* s) {
    const int TSTATES_PER_FRAME  = ULA_TSTATES_PER_LINE * ULA_LINES_PER_FRAME; // 69888
    const int TSTATES_PER_SAMPLE = TSTATES_PER_FRAME / 882;

    int cycles_done      = 0;
    int next_sample_at   = TSTATES_PER_SAMPLE;
    int ula_line         = 0;
    int line_cycles_done = 0;

    // Pre-rellenar border_lines con el color actual.  Se sobreescribirá línea
    // a línea cuando el Z80 ejecute OUT (0xFE), <valor>.
    for (int i = 0; i < ULA_LINES_PER_FRAME; i++)
        s->border_lines[i] = border_color;

    z80_pulse_irq(&s->cpu, 0xFF);

    while (cycles_done < TSTATES_PER_FRAME) {
        int cycles = (int)z80_step(&s->cpu);
        cycles_done      += cycles;
        line_cycles_done += cycles;

        // ── Procesar escritura en puerto ULA ─────────────────────────────
        if (ula_out_dirty) {
            border_color  = ula_pending_out & 0x07;
            ear_bit       = (ula_pending_out & 0x10) >> 4;
            ula_out_dirty = false;
            // Propagar color al resto de líneas del frame desde la actual
            for (int i = ula_line; i < ULA_LINES_PER_FRAME; i++)
                s->border_lines[i] = border_color;
        }

        // ── Avanzar línea(s) ULA ─────────────────────────────────────────
        while (line_cycles_done >= ULA_TSTATES_PER_LINE) {
            line_cycles_done -= ULA_TSTATES_PER_LINE;
            ula_line++;
            if (ula_line < ULA_LINES_PER_FRAME)
                s->border_lines[ula_line] = border_color;
        }

        // ── Avanzar reproductor de cinta activo ──────────────────────────
        switch (s->tape_src) {
        case TAPE_TAP:
            tap_update(&s->tap, cycles);
            mic_bit = s->tap.ear;
            break;
        case TAPE_TZX:
            tzx_update(&s->tzx, cycles);
            mic_bit = s->tzx.ear;
            break;
        default:
            break;
        }

        // ── Muestreo de audio (beeper + cinta) ───────────────────────────
        while (cycles_done >= next_sample_at) {
            if (s->audio_pos < 882) {
                float level = 0.0f;
                if (ear_bit) level += 0.15f;   // beeper (OUT 0xFE bit 4)
                if (mic_bit) level += 0.15f;   // cinta  (EAR de entrada)
                if (level == 0.0f) level = -0.2f;
                s->audio_buffer[s->audio_pos++] = level;
            }
            next_sample_at += TSTATES_PER_SAMPLE;
        }
    }

    if (s->audio_dev > 0 && s->audio_pos > 0) {
        SDL_QueueAudio(s->audio_dev, s->audio_buffer, s->audio_pos * sizeof(float));
        s->audio_pos = 0;
    }
    s->frame_counter++;
}

// =============================================================================
// Renderizado completo: borde por línea + imagen 256×192
// =============================================================================
//
// El framebuffer (FULL_W × FULL_H = 352 × 288) se estructura:
//
//   fb_row  0.. 47  → borde superior  (48 px)
//   fb_row 48..239  → imagen + borde izq/dcha
//   fb_row 240..287 → borde inferior  (48 px)
//
// Mapeo fb_row → línea ULA:
//   fb_row + ULA_FIRST_VISIBLE_LINE (16)
//   → fb_row=0  : ULA 16  (primera línea borde superior visible)
//   → fb_row=47 : ULA 63  (última línea borde superior)
//   → fb_row=48 : ULA 64  (primera línea de imagen)
//   → fb_row=239: ULA 255 (última línea de imagen)
//   → fb_row=240: ULA 256 (primera línea borde inferior)
//   → fb_row=287: ULA 303 (última línea visible)
// =============================================================================

void spectrum_render(ZXSpectrum* s) {
    bool flash_phase = (s->frame_counter & 16) != 0;

    for (int fb_row = 0; fb_row < FULL_H; fb_row++) {
        int      ula_line  = ULA_FIRST_VISIBLE_LINE + fb_row;
        uint32_t border_px = palette[s->border_lines[ula_line] & 0x07];
        uint32_t* dst      = &s->framebuffer[fb_row * FULL_W];

        if (fb_row >= BORDER_TOP && fb_row < BORDER_TOP + SCREEN_H) {
            // ── Fila con imagen ───────────────────────────────────────────
            int img_row = fb_row - BORDER_TOP;  // 0..191

            // Borde izquierdo
            for (int x = 0; x < BORDER_LEFT; x++)
                dst[x] = border_px;

            // Imagen: desentrelazado de la VRAM (orden de tercio/línea/carácter)
            int vram_y = (img_row & 0xC0)
                       | ((img_row & 0x07) << 3)
                       | ((img_row & 0x38) >> 3);
            for (int col = 0; col < 32; col++) {
                uint8_t pixels = s->memory[0x4000 + vram_y * 32 + col];
                uint8_t attr   = s->memory[0x5800 + (img_row / 8) * 32 + col];
                bool bright    = (attr & 0x40) != 0;
                bool flash     = (attr & 0x80) != 0;
                int ink   = (attr & 0x07) | (bright ? 8 : 0);
                int paper = ((attr >> 3) & 0x07) | (bright ? 8 : 0);
                if (flash && flash_phase) { int t = ink; ink = paper; paper = t; }
                for (int b = 0; b < 8; b++) {
                    bool is_ink = (pixels & (0x80 >> b)) != 0;
                    dst[BORDER_LEFT + col * 8 + b] = palette[is_ink ? ink : paper];
                }
            }

            // Borde derecho
            for (int x = BORDER_LEFT + SCREEN_W; x < FULL_W; x++)
                dst[x] = border_px;

        } else {
            // ── Fila de borde puro (superior o inferior) ──────────────────
            for (int x = 0; x < FULL_W; x++)
                dst[x] = border_px;
        }
    }

    SDL_UpdateTexture(s->texture, NULL, s->framebuffer, FULL_W * sizeof(uint32_t));
    SDL_RenderClear(s->renderer);
    SDL_Rect dst_rect = { 0, 0, FULL_W * 2, FULL_H * 2 };
    SDL_RenderCopy(s->renderer, s->texture, NULL, &dst_rect);
    SDL_RenderPresent(s->renderer);
}

// =============================================================================
// Detección de extensión (case-insensitive, últimos 4 caracteres)
// =============================================================================
static bool ext_eq(const char* path, const char* ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char* tail = path + plen - elen;
    for (size_t i = 0; i < elen; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
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

        if (ext_eq(file, ".tap")) {
            if (spectrum_load_tap(&spec, file) == 0)
                printf("TAP cargado. Escribe LOAD \"\" y pulsa F1 para iniciar.\n");
            else
                fprintf(stderr, "Error cargando TAP: %s\n", file);

        } else if (ext_eq(file, ".tzx")) {
            if (spectrum_load_tzx(&spec, file) == 0)
                printf("TZX cargado. Escribe LOAD \"\" y pulsa F1 para iniciar.\n");
            else
                fprintf(stderr, "Error cargando TZX: %s\n", file);

        } else if (ext_eq(file, ".sna")) {
            if (spectrum_load_sna(&spec, file) == 0)
                printf("Snapshot SNA cargado: %s\n", file);
            else
                fprintf(stderr, "Error cargando SNA: %s\n", file);

        } else {
            fprintf(stderr, "Formato no reconocido. Formatos admitidos: .sna  .tap  .tzx\n");
        }
    } else {
        printf("Uso: %s <archivo.sna|archivo.tap|archivo.tzx>\n", argv[0]);
        printf("  F1 → iniciar/rebobinar cinta\n");
        printf("  F2 → detener cinta\n");
    }

    const uint32_t FRAME_MS = 1000 / 50;  // 20 ms por frame (50 Hz)

    while (!spec.quit) {
        uint32_t t0 = SDL_GetTicks();

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

        uint32_t elapsed = SDL_GetTicks() - t0;
        if (elapsed < FRAME_MS)
            SDL_Delay(FRAME_MS - elapsed);
    }

    spectrum_destroy(&spec);
    return 0;
}
