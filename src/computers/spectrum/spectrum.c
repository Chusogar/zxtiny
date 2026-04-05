#include "spectrum.h"
#include <SDL.h>

uint32_t spectrum_palette[16] = {
    0xff000000, /* negro */
    0xff0000bf, /* azul */
    0xffbf0000, /* rojo */
    0xffbf00bf, /* magenta */
    0xff00bf00, /* verde */
    0xff00bfbf, /* ciano */
    0xffbfbf00, /* amarillo */
    0xffbfbfbf, /* blanco */
    0xff000000, /* negro brillante */
    0xff0000ff, /* azul brillante */
    0xffff0000, /* rojo brillante */
    0xffff00ff, /* magenta brillante */
    0xff00ff00, /* verde brillante */
    0xff00ffff, /* ciano brillante */
    0xffffff00, /* amarillo brillante */
    0xffffffff  /* blanco brillante */
};

// ═══════════════════════════════════════════════════════════════════
// VARIABLES GLOBALES PARA AUDIO SINCRONIZADO
// ═══════════════════════════════════════════════════════════════════
static double audio_sample_accumulator = 0.0;  // Acumula muestras fraccionarias
static int cycles_since_last_audio_update = 0; // Ciclos desde la última actualización de audio

// ═══════════════════════════════════════════════════════════════════
// LECTURA Y ESCRITURA DE MEMORIA
// ═══════════════════════════════════════════════════════════════════

static uint8_t rb(void* userdata, uint16_t addr) {
  spectrum* const p = (spectrum*) userdata;
  addr &= 0xffff;

  if (addr < 0x4000) {
    return p->rom[addr];
  } else {
    return p->ram[addr - 0x4000];
  }
  return 0x00;
}

static void wb(void* userdata, uint16_t addr, uint8_t val) {
  spectrum* const p = (spectrum*) userdata;
  addr &= 0xffff;

  if (addr < 0x4000) {
    // cannot write to rom
  } else {
    p->ram[addr - 0x4000] = val;
  }
}

// ═══════════════════════════════════════════════════════════════════
// MANEJO DE TECLADO
// ═══════════════════════════════════════════════════════════════════

void spectrum_key_press(void* userdata, int _key_code, bool _pressed) {
    spectrum* const p = (spectrum*) userdata;
    int row = -1, bit = -1;

    switch (_key_code) {
        case SDLK_a:     row=1; bit=0; break;
        case SDLK_b:     row=7; bit=4; break;
        case SDLK_c:     row=0; bit=3; break;
        case SDLK_d:     row=1; bit=2; break;
        case SDLK_e:     row=2; bit=2; break;
        case SDLK_f:     row=1; bit=3; break;
        case SDLK_g:     row=1; bit=4; break;
        case SDLK_h:     row=6; bit=4; break;
        case SDLK_i:     row=5; bit=2; break;
        case SDLK_j:     row=6; bit=3; break;
        case SDLK_k:     row=6; bit=2; break;
        case SDLK_l:     row=6; bit=1; break;
        case SDLK_m:     row=7; bit=2; break;
        case SDLK_n:     row=7; bit=3; break;
        case SDLK_o:     row=5; bit=1; break;
        case SDLK_p:     row=5; bit=0; break;
        case SDLK_q:     row=2; bit=0; break;
        case SDLK_r:     row=2; bit=3; break;
        case SDLK_s:     row=1; bit=1; break;
        case SDLK_t:     row=2; bit=4; break;
        case SDLK_u:     row=5; bit=3; break;
        case SDLK_v:     row=0; bit=4; break;
        case SDLK_w:     row=2; bit=1; break;
        case SDLK_x:     row=0; bit=2; break;
        case SDLK_y:     row=5; bit=4; break;
        case SDLK_z:     row=0; bit=1; break;
        case SDLK_0:     row=4; bit=0; break;
        case SDLK_1:     row=3; bit=0; break;
        case SDLK_2:     row=3; bit=1; break;
        case SDLK_3:     row=3; bit=2; break;
        case SDLK_4:     row=3; bit=3; break;
        case SDLK_5:     row=3; bit=4; break;
        case SDLK_6:     row=4; bit=4; break;
        case SDLK_7:     row=4; bit=3; break;
        case SDLK_8:     row=4; bit=2; break;
        case SDLK_9:     row=4; bit=1; break;
        case SDLK_SPACE: row=7; bit=0; break;
        case SDLK_RETURN:row=6; bit=0; break;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT: row=0; bit=0; break; // Caps Shift
        case SDLK_LCTRL:
        case SDLK_RCTRL:  row=7; bit=1; break; // Symbol Shift
    }

    if (row >= 0 && bit >= 0) {
        if (_pressed)    p->keyboard[row] &= ~(1 << bit);
        else            p->keyboard[row] |=  (1 << bit);
    }
}

// ═══════════════════════════════════════════════════════════════════
// FUNCIÓN PRINCIPAL DE AUDIO SINCRONIZADO
// ═══════════════════════════════════════════════════════════════════
/**
* Genera audio sincronizado basado en ciclos reales de CPU.
* Esta función mantiene sincronización precisa entre el CPU y el audio
* usando un acumulador de muestras fraccionarias.
*/
static void generate_audio_for_cycles(spectrum* const p, int num_cycles) {
    if (!p->sound_enabled || p->mute_audio || !p->push_sample) {
        return;
    }

    // Calcula cuántas muestras deben generarse para estos ciclos
    // usando la fórmula: (ciclos * frecuencia_muestreo) / frecuencia_cpu
    double samples_per_cycle = (double)SPECTRUM_SAMPLE_RATE / (double)SPECTRUM_CLOCK_SPEED;
    audio_sample_accumulator += (double)num_cycles * samples_per_cycle;

    // Genera las muestras completadas
    int16_t sample_value = p->current_speaker_level ? 8000 : -8000;
    
    while (audio_sample_accumulator >= 1.0) {
        p->push_sample(p, sample_value);
        audio_sample_accumulator -= 1.0;
    }
}

// ═══════════════════════════════════════════════════════════════════
// PUERTOS I/O - Z80_JGZ80
// ═══════════════════════════════════════════════════════════════════
#ifdef Z80_JGZ80

static uint8_t port_in(z80* const z, uint16_t port) {
    spectrum* const p = (spectrum*) z->userdata;
    uint8_t hiport = port >> 8;
    uint8_t res = 0xff;

    if ((port & 1) == 0) { // ULA
        res = 0xbf;
        uint8_t hi = port >> 8;

        // Teclado
        for (int r = 0; r < 8; r++)
            if ((hi & (1 << r)) == 0)
                res &= p->keyboard[r];
    }

    return res;
}

static void port_out(z80* const z, uint16_t port, uint8_t val) {
    spectrum* const p = (spectrum*) z->userdata;

    if ((port & 1) == 0) {
        p->border_color   = val & 0x07;
        p->last_fe_write  = val;

        // El bit 4 controla el altavoz
        uint8_t new_speaker_level = (val & 0x10) ? 1 : 0;
        
        // Solo generar audio si cambió el nivel del speaker
        if (new_speaker_level != p->current_speaker_level) {
            generate_audio_for_cycles(p, 1);
        }
        
        p->current_speaker_level = new_speaker_level;
    }

    // setting the interrupt vector
    if (port == 0) {
        p->int_vector = val;
    }
}

#endif // Z80_JGZ80

// ═══════════════════════════════════════════════════════════════════
// PUERTOS I/O - Z80_SZ_Z80
// ═══════════════════════════════════════════════════════════════════
#ifdef Z80_SZ_Z80

static uint8_t port_in(z80* const z, uint8_t port) {
    spectrum* const p = (spectrum*) z->userdata;
    uint8_t hiport = port >> 8;
    uint8_t res = 0xff;

    if ((port & 1) == 0) { // ULA
        res = 0xbf;
        uint8_t hi = port >> 8;

        // Teclado
        for (int r = 0; r < 8; r++)
            if ((hi & (1 << r)) == 0)
                res &= p->keyboard[r];
    }

    return res;
}

static void port_out(z80* const z, uint8_t port, uint8_t val) {
    spectrum* const p = (spectrum*) z->userdata;

    if ((port & 1) == 0) {
        p->border_color   = val & 0x07;
        p->last_fe_write  = val;

        // El bit 4 controla el altavoz
        uint8_t new_speaker_level = (val & 0x10) ? 1 : 0;
        
        // Solo generar audio si cambió el nivel del speaker
        if (new_speaker_level != p->current_speaker_level) {
            generate_audio_for_cycles(p, 1);
        }
        
        p->current_speaker_level = new_speaker_level;
    }

    // setting the interrupt vector
    if (port == 0) {
        p->int_vector = val;
    }
}

#endif // Z80_SZ_Z80

// ═══════════════════════════════════════════════════════════════════
// FUNCIONES AUXILIARES DE ARCHIVOS
// ═══════════════════════════════════════════════════════════════════

static inline char* append_path(const char* s1, const char* s2) {
    const int buf_size = strlen(s1) + strlen(s2) + 1;
    char* path = calloc(buf_size, sizeof(char));
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, buf_size, "%s%s", s1, s2);
    return path;
}

static inline int load_file(
    const char* filename, uint8_t* memory, size_t nb_bytes) {
    FILE* f = fopen(filename, "rb");
    if (f == NULL) {
        fprintf(stderr, "error: can't open file '%s'.\n", filename);
        return 1;
    }

    size_t result = fread(memory, sizeof(uint8_t), nb_bytes, f);
    if (result != nb_bytes) {
        fprintf(stderr, "error: while reading file '%s'\n", filename);
        return 1;
    }

    fclose(f);
    return 0;
}

// ═══════════════════════════════════════════════════════════════════
// GRÁFICOS
// ═══════════════════════════════════════════════════════════════════

static inline void get_color(spectrum* const p, uint8_t color_no, uint8_t* r, uint8_t* g, uint8_t* b) {
    // Placeholder para expansión futura
}

static inline void get_palette(spectrum* const p, uint8_t pal_no, uint8_t* pal) {
    // Placeholder para expansión futura
}

int get_pixel_address(int x, int y) {
    int y76 = y & 0b11000000;
    int y53 = y & 0b00111000;
    int y20 = y & 0b00000111;
    int address = (y76 << 5) + (y20 << 8) + (y53 << 2) + (x >> 3);
    return address;
}

int get_attribute_address(int x, int y) {
    int y73 = y & 0b11111000;
    int address = (y73 << 2) + (x >> 3);
    return address;
}

uint32_t getPaletteColor(int color_index) {
    return spectrum_palette[color_index];
}

static inline void spectrum_draw(spectrum* const p) {
    int x, y;
    int _posi = 0;
    
    for (y = 0; y < SPECTRUM_SCREEN_HEIGHT; y++) {
        for (x = 0; x < SPECTRUM_SCREEN_WIDTH; x++) {
            int byte_pos = get_pixel_address(x, y);
            int bit_pos = (y * SPECTRUM_SCREEN_WIDTH + x) % 8;
            int _bit_is_set = ((p->ram[byte_pos]) >> (7 - (x % 8))) & 1;
            int _attr = p->ram[get_attribute_address(x, y) + 6144];
            int _ink = (int)(_attr & 0b0111);
            int _paper = ((_attr & 0x38) / 8);

            int color_index = _bit_is_set ? _ink : _paper;
            uint32_t color = getPaletteColor(color_index);

            p->screen_buffer[_posi++] = color;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// AUDIO - FUNCIÓN ACTUALIZADA
// ═══════════════════════════════════════════════════════════════════
/**
* sound_update - Actualiza el audio al final de cada frame.
* 
* Esta función se llama una vez por frame (después de ejecutar
* SPECTRUM_CYCLES_PER_FRAME ciclos del CPU).
* 
* El audio ya ha sido generado de manera sincronizada en generate_audio_for_cycles()
* llamada cada vez que cambia el nivel del speaker. 
*/
static inline void sound_update(spectrum* const p) {
    if (!p->sound_enabled || p->mute_audio) {
        return;
    }

    // El audio ya ha sido procesado en tiempo real por generate_audio_for_cycles()
    // Esta función actúa como un checkpoint para sincronización de frames
    cycles_since_last_audio_update = 0;
}

// ═══════════════════════════════════════════════════════════════════
// CARGA DE SNAPSHOTS .SNA
// ═══════════════════════════════════════════════════════════════════

bool load_sna(spectrum* const p, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) { 
        fprintf(stderr, "No se pudo abrir .sna: %s\n", filename); 
        return false; 
    }

    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) { 
        fclose(f); 
        fprintf(stderr, "Archivo .sna incompleto (header)\n"); 
        return false; 
    }

    // Restaurar registros Z80 (formato .sna 48K)
    p->cpu.i      = header[0];
    p->cpu.h_l_   = (header[2] << 8) | header[1];
    p->cpu.d_e_   = (header[4] << 8) | header[3];
    p->cpu.b_c_   = (header[6] << 8) | header[5];
    p->cpu.a_f_   = (header[8] << 8) | header[7];
    p->cpu.hl     = (header[10] << 8) | header[9];
    p->cpu.de     = (header[12] << 8) | header[11];
    p->cpu.bc     = (header[14] << 8) | header[13];
    p->cpu.iy     = (header[16] << 8) | header[15];
    p->cpu.ix     = (header[18] << 8) | header[17];
    p->cpu.iff2   = header[19] ? 1 : 0;
    p->cpu.r      = header[20];
    p->cpu.af     = (header[22] << 8) | header[21];
    p->cpu.sp     = (header[24] << 8) | header[23];
    p->cpu.interrupt_mode = header[25];
    p->border_color = header[26] & 0x07;

    if (fread(&p->ram[0], 1, 49152, f) != 49152) { 
        fclose(f); 
        fprintf(stderr, "Archivo .sna incompleto (RAM)\n"); 
        return false; 
    }
    fclose(f);

    uint16_t sp = p->cpu.sp;
    p->cpu.pc = (p->cpu.read_byte(p, sp + 1) << 8) | p->cpu.read_byte(p, sp);
    p->cpu.sp += 2;

    p->cpu.iff1 = p->cpu.iff2;

    printf("Snapshot .sna cargado: %s\n", filename);
    printf("PC=0x%04X SP=0x%04X Border=%d IM=%d\n", 
           p->cpu.pc, p->cpu.sp, p->border_color, p->cpu.interrupt_mode);

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// INICIALIZACIÓN DEL EMULADOR
// ═══════════════════════════════════════════════════════════════════

int spectrum_init(spectrum* const p, const char* rom_dir) {
    z80_init(&p->cpu);

    p->cpu.userdata = p;
    p->cpu.read_byte = rb;
    p->cpu.write_byte = wb;
    p->cpu.port_in = port_in;
    p->cpu.port_out = port_out;

    p->border_color   = 7;
    p->current_speaker_level = 0;

    for (int _i = 0; _i < 8; _i++) {
        p->keyboard[_i] = 0xff;
    }

    memset(p->rom, 0, sizeof(p->rom));
    memset(p->ram, 0, sizeof(p->ram));
    memset(p->screen_buffer, 0, sizeof(p->screen_buffer));

    p->int_vector = 0;
    p->vblank_enabled = 1;
    p->sound_enabled = 1;
    p->audio_frame_pos = 0;
    p->flip_screen = 0;

    // Cargar ROM
    int r = 0;
    char* file0 = append_path(rom_dir, "spectrum.rom");
    printf("Ruta Total: %s\n", file0);
    r += load_file(file0, &p->rom[0x0000], 0x4000);
    free(file0);

    p->update_screen = NULL;

    // ═══════════════════════════════════════════════════════════════
    // INICIALIZACIÓN DE AUDIO - TAMAÑO DE BUFFER AUMENTADO
    // ═══════════════════════════════════════════════════════════════
    // Calcula el número de muestras necesarias por frame a 44.1 kHz y 60 FPS
    // 44100 / 60 = 735 muestras por frame
    p->audio_buffer_len = SPECTRUM_SAMPLE_RATE / SPECTRUM_FPS;
    p->audio_buffer = calloc(p->audio_buffer_len, sizeof(int16_t));
    
    if (p->audio_buffer == NULL) {
        fprintf(stderr, "Error: No se pudo asignar memoria para el buffer de audio\n");
        return 1;
    }

    p->sample_rate = SPECTRUM_SAMPLE_RATE;
    p->mute_audio = false;
    p->push_sample = NULL;

    // Resetear variables de audio
    audio_sample_accumulator = 0.0;
    cycles_since_last_audio_update = 0;

    printf("Audio inicializado: %d Hz, %d muestras por frame\n", 
           p->sample_rate, p->audio_buffer_len);

    return r != 0;
}

void init_palette(spectrum* const p) {
    for (int _i = 0; _i < 16; _i++) {
        p->palette[_i] = spectrum_palette[_i];
    }
}

void spectrum_quit(spectrum* const p) {
    if (p->audio_buffer != NULL) {
        free(p->audio_buffer);
        p->audio_buffer = NULL;
    }
}

// Variable global para conteo de ciclos (Z80_JGZ80)
int cpu_cyc = 0;

// ═══════════════════════════════════════════════════════════════════
// LOOP PRINCIPAL DE EMULACIÓN
// ═══════════════════════════════════════════════════════════════════
/**
* spectrum_update - Actualiza el emulador para un tiempo específico en ms.
* 
* La emulación se ejecuta en pasos de frame:
* - Ejecuta SPECTRUM_CYCLES_PER_FRAME ciclos del CPU
* - El audio se genera sincronizadamente en cada OUT a puerto 0xFE
* - Al final de cada frame, se llama a sound_update() para resetear contadores
*/
void spectrum_update(spectrum* const p, unsigned int ms) {
    int count = 0;
    
    while (count < ms * SPECTRUM_CLOCK_SPEED / 1000) {
        
#ifdef Z80_SZ_Z80
        int cyc = p->cpu.cyc;
        z80_step(&p->cpu);
        int elapsed = p->cpu.cyc - cyc;
        count += elapsed;
        cycles_since_last_audio_update += elapsed;

        if (p->cpu.cyc >= SPECTRUM_CYCLES_PER_FRAME) {
            p->cpu.cyc -= SPECTRUM_CYCLES_PER_FRAME;

            if (p->vblank_enabled) {
                z80_gen_int(&p->cpu, p->int_vector);

                spectrum_draw(p);
                if (p->update_screen != NULL) {
                    p->update_screen(p);
                }
                
                sound_update(p);
            }
        }
#endif

#ifdef Z80_JGZ80
        int cyc = cpu_cyc;
        int _cyc_done = z80_step(&p->cpu);
        cpu_cyc += _cyc_done;
        int elapsed = cpu_cyc - cyc;
        count += elapsed;
        cycles_since_last_audio_update += elapsed;

        if (cpu_cyc >= SPECTRUM_CYCLES_PER_FRAME) {
            cpu_cyc -= SPECTRUM_CYCLES_PER_FRAME;

            if (p->vblank_enabled) {
                z80_pulse_irq(&p->cpu, 1);

                spectrum_draw(p);
                if (p->update_screen != NULL) {
                    p->update_screen(p);
                }
                
                sound_update(p);
            }
        }
#endif

    }
}

