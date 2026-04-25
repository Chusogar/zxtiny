#include "zx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ZXSpectrum spec;

// Paleta de colores estándar del ZX Spectrum (ARGB8888)
static const uint32_t palette[16] = {
    0xFF000000, 0xFF0000D7, 0xFFD70000, 0xFFD700D7, 0xFF00D700, 0xFF00D7D7, 0xFFD7D700, 0xFFD7D7D7, // Normal
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF  // Brillo
};

// --- Callbacks de Memoria e I/O para JGZ80 ---
static uint8_t mem_read(void* userdata, uint16_t addr) {
    ZXSpectrum* spec = (ZXSpectrum*)userdata;
    return spec->memory[addr];
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    ZXSpectrum* spec = (ZXSpectrum*)userdata;
    if (addr >= 0x4000) { // La ROM (0x0000 - 0x3FFF) está protegida contra escrituras
        spec->memory[addr] = val;
    }
}

static uint8_t port_in(z80* z, uint16_t port) {
    //ZXSpectrum* spec = (ZXSpectrum*)userdata;
	(void)z;
    // La ULA lee el teclado a través de los puertos pares (bit 0 = 0)
    if ((port & 0x01) == 0) {
        uint8_t result = 0xFF; // Todas las teclas levantadas por defecto
        for (int i = 0; i < 8; i++) {
            if ((port & (1 << (i + 8))) == 0) { // Comprobar bus de direcciones alto (A8-A15)
                result &= /*spec->*/keyboard_matrix[i];
            }
        }
        return result;
    }
    return 0xFF; 
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    //ZXSpectrum* spec = (ZXSpectrum*)userdata;
	(void)z;
    // La ULA responde a los puertos pares
    if ((port & 0x01) == 0) {
        /*spec->*/border_color = val & 0x07;         // Bits 0-2: Color del borde
        /*spec->*/ear_bit = (val & 0x10) >> 4;       // Bit 4: Pin EAR (Altavoz)
    }
}
// ---------------------------------------------

void spectrum_init(ZXSpectrum* spec) {
    memset(spec, 0, sizeof(ZXSpectrum));
    for (int i = 0; i < 8; i++) /*spec->*/keyboard_matrix[i] = 0xFF;

	z80_init(&spec->cpu);
    
    // Configuración del Core Z80
    spec->cpu.userdata = spec;
    spec->cpu.read_byte = mem_read;
    spec->cpu.write_byte = mem_write;
    spec->cpu.port_in = port_in;
    spec->cpu.port_out = port_out;
    
    //z80_power(&spec->cpu, true); // Enciende y hace reset al Z80
	z80_reset(&spec->cpu);
    
    // Inicialización de SDL2
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    
    // Ventana escalada x2 con margen para mostrar el color del borde
    spec->window = SDL_CreateWindow("ZX Spectrum 48K Emulador", 
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                    640, 480, SDL_WINDOW_SHOWN);
    spec->renderer = SDL_CreateRenderer(spec->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    spec->texture = SDL_CreateTexture(spec->renderer, SDL_PIXELFORMAT_ARGB8888, 
                                      SDL_TEXTUREACCESS_STREAMING, 256, 192);
    
    // Configuración del subsistema de Audio
    SDL_AudioSpec wanted, have;
    SDL_zero(wanted);
    wanted.freq = 44100;
    wanted.format = AUDIO_F32;
    wanted.channels = 1;
    wanted.samples = 1024;
    spec->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &have, 0);
    if (spec->audio_dev > 0) SDL_PauseAudioDevice(spec->audio_dev, 0);
}

void spectrum_destroy(ZXSpectrum* spec) {
    SDL_DestroyTexture(spec->texture);
    SDL_DestroyRenderer(spec->renderer);
    SDL_DestroyWindow(spec->window);
    SDL_CloseAudioDevice(spec->audio_dev);
    SDL_Quit();
}

// Cargar la ROM obligatoria de 16KB del 48K
int spectrum_load_rom(ZXSpectrum* spec, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    fread(spec->memory, 1, 16384, f);
    fclose(f);
    return 0;
}

// Carga de snapshots SNA
int spectrum_load_sna(ZXSpectrum* spec, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    
    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) { fclose(f); return -1; }
    if (fread(spec->memory + 0x4000, 1, 49152, f) != 49152) { fclose(f); return -1; }
    fclose(f);
    
    // Restituir registros desde la cabecera SNA a la estructura jgz80
    spec->cpu.i = header[0];
    spec->cpu.l_ = header[1]; spec->cpu.h_ = header[2];
    spec->cpu.e_ = header[3]; spec->cpu.d_ = header[4];
    spec->cpu.c_ = header[5]; spec->cpu.b_ = header[6];
    spec->cpu.f_ = header[7]; spec->cpu.a_ = header[8];
    
    spec->cpu.l = header[9]; spec->cpu.h = header[10];
    spec->cpu.e = header[11]; spec->cpu.d = header[12];
    spec->cpu.c = header[13]; spec->cpu.b = header[14];
    spec->cpu.iy = header[15] | (header[16] << 8);
    spec->cpu.ix = header[17] | (header[18] << 8);
    
    uint8_t iff2 = (header[19] & 0x04) != 0;
    spec->cpu.iff2 = iff2;
    spec->cpu.iff1 = iff2;
    spec->cpu.r = header[20];
    spec->cpu.f = header[21]; spec->cpu.a = header[22];
    spec->cpu.sp = header[23] | (header[24] << 8);
    spec->cpu.interrupt_mode = header[25];
    /*spec->*/border_color = header[26];
    
    // En el formato SNA, el PC se encuentra apilado en la memoria
    uint16_t sp = spec->cpu.sp;
    spec->cpu.pc = spec->memory[sp] | (spec->memory[sp+1] << 8);
    spec->cpu.sp += 2;
    
    return 0;
}

void spectrum_handle_key(ZXSpectrum* spec, SDL_Scancode key, bool pressed) {
    uint8_t row = 255, bit = 255;
    switch(key) {
        case SDL_SCANCODE_LSHIFT: row=0; bit=0; break;
        case SDL_SCANCODE_Z:      row=0; bit=1; break;
        case SDL_SCANCODE_X:      row=0; bit=2; break;
        case SDL_SCANCODE_C:      row=0; bit=3; break;
        case SDL_SCANCODE_V:      row=0; bit=4; break;
        
        case SDL_SCANCODE_A: row=1; bit=0; break;
        case SDL_SCANCODE_S: row=1; bit=1; break;
        case SDL_SCANCODE_D: row=1; bit=2; break;
        case SDL_SCANCODE_F: row=1; bit=3; break;
        case SDL_SCANCODE_G: row=1; bit=4; break;
        
        case SDL_SCANCODE_Q: row=2; bit=0; break;
        case SDL_SCANCODE_W: row=2; bit=1; break;
        case SDL_SCANCODE_E: row=2; bit=2; break;
        case SDL_SCANCODE_R: row=2; bit=3; break;
        case SDL_SCANCODE_T: row=2; bit=4; break;
        
        case SDL_SCANCODE_1: row=3; bit=0; break;
        case SDL_SCANCODE_2: row=3; bit=1; break;
        case SDL_SCANCODE_3: row=3; bit=2; break;
        case SDL_SCANCODE_4: row=3; bit=3; break;
        case SDL_SCANCODE_5: row=3; bit=4; break;
        
        case SDL_SCANCODE_0: row=4; bit=0; break;
        case SDL_SCANCODE_9: row=4; bit=1; break;
        case SDL_SCANCODE_8: row=4; bit=2; break;
        case SDL_SCANCODE_7: row=4; bit=3; break;
        case SDL_SCANCODE_6: row=4; bit=4; break;
        
        case SDL_SCANCODE_P: row=5; bit=0; break;
        case SDL_SCANCODE_O: row=5; bit=1; break;
        case SDL_SCANCODE_I: row=5; bit=2; break;
        case SDL_SCANCODE_U: row=5; bit=3; break;
        case SDL_SCANCODE_Y: row=5; bit=4; break;
        
        case SDL_SCANCODE_RETURN: row=6; bit=0; break;
        case SDL_SCANCODE_L:      row=6; bit=1; break;
        case SDL_SCANCODE_K:      row=6; bit=2; break;
        case SDL_SCANCODE_J:      row=6; bit=3; break;
        case SDL_SCANCODE_H:      row=6; bit=4; break;
        
        case SDL_SCANCODE_SPACE:  row=7; bit=0; break;
        case SDL_SCANCODE_RSHIFT: row=7; bit=1; break; // Se usa RSHIFT como Symbol Shift
        case SDL_SCANCODE_M:      row=7; bit=2; break;
        case SDL_SCANCODE_N:      row=7; bit=3; break;
        case SDL_SCANCODE_B:      row=7; bit=4; break;
        default: break;
    }
    
    if (row != 255) {
        if (pressed) /*spec->*/keyboard_matrix[row] &= ~(1 << bit); // 0 = Pulsado
        else         /*spec->*/keyboard_matrix[row] |= (1 << bit);  // 1 = Levantado
    }
}

void spectrum_run_frame(ZXSpectrum* spec) {
    const int t_states_per_frame = 69888;
    const int t_states_per_sample = t_states_per_frame / 882;
    int cycles_this_frame = 0;
    int next_sample_cycle = t_states_per_sample;
    
    // Dispara VBLANK IRQ al empezar el dibujado del frame
    //z80_interrupt(&spec->cpu, 0xFF);
	z80_pulse_irq(&spec->cpu, 0xFF);
    
    while (cycles_this_frame < t_states_per_frame) {
        int cycles = (int)z80_step(&spec->cpu);
        cycles_this_frame += cycles;
        
        // Muestrear el beeper sincronizadamente a la ejecución del Z80
        while (cycles_this_frame >= next_sample_cycle) {
            if (spec->audio_pos < 882) {
                spec->audio_buffer[spec->audio_pos++] = /*spec->*/ear_bit ? 0.2f : -0.2f;
            }
            next_sample_cycle += t_states_per_sample;
        }
    }
    
    if (spec->audio_dev > 0 && spec->audio_pos > 0) {
        SDL_QueueAudio(spec->audio_dev, spec->audio_buffer, spec->audio_pos * sizeof(float));
        spec->audio_pos = 0;
    }
    spec->frame_counter++;
}

void spectrum_render(ZXSpectrum* spec) {
    bool flash_phase = (spec->frame_counter & 16) != 0; // Invierte el parpadeo cada 16 frames
    
    for (int y = 0; y < 192; y++) {
        // La memoria de video del Spectrum está intercalada en tercios
        int vram_y = (y & 0xC0) | ((y & 0x07) << 3) | ((y & 0x38) >> 3);
        
        for (int x = 0; x < 32; x++) {
            uint8_t pixels = spec->memory[0x4000 + vram_y * 32 + x];
            uint8_t attr = spec->memory[0x5800 + (y / 8) * 32 + x];
            
            bool bright = (attr & 0x40) != 0;
            bool flash = (attr & 0x80) != 0;
            
            int ink = (attr & 0x07) | (bright ? 8 : 0);
            int paper = ((attr & 0x38) >> 3) | (bright ? 8 : 0);
            
            if (flash && flash_phase) {
                int temp = ink; ink = paper; paper = temp;
            }
            
            for (int b = 0; b < 8; b++) {
                bool is_ink = (pixels & (0x80 >> b)) != 0;
                spec->screen_buffer[y * 256 + (x * 8 + b)] = palette[is_ink ? ink : paper];
            }
        }
    }
    
    SDL_UpdateTexture(spec->texture, NULL, spec->screen_buffer, 256 * sizeof(uint32_t));
    
    // Pintar el color del borde como color de fondo
    uint32_t border = palette[/*spec->*/border_color];
    SDL_SetRenderDrawColor(spec->renderer, (border >> 16) & 0xFF, (border >> 8) & 0xFF, border & 0xFF, 255);
    SDL_RenderClear(spec->renderer);
    
    // Pegar la textura 256x192 centrada para simular el borde original
    SDL_Rect dst_rect = { 64, 48, 512, 384 };
    SDL_RenderCopy(spec->renderer, spec->texture, NULL, &dst_rect);
    SDL_RenderPresent(spec->renderer);
}

int main(int argc, char* argv[]) {
    
    spectrum_init(&spec);
    
    // El emulador necesita las rutinas y tipografía de la ROM original de Sinclair
    if (spectrum_load_rom(&spec, "48.rom") != 0) {
        printf("Aviso: '48.rom' no encontrada. Ponla en el mismo directorio que el ejecutable.\n");
    }
    
    if (argc > 1) {
        if (spectrum_load_sna(&spec, argv[1]) == 0) {
            printf("Snapshot cargado correctamente: %s\n", argv[1]);
        } else {
            printf("Error leyendo el archivo SNA: %s\n", argv[1]);
        }
    } else {
        printf("Sintaxis: %s [archivo.sna]\n", argv[0]);
    }
    
    uint32_t frame_ticks = 1000 / 50; // 50 Hz (20ms/frame)
    
    while (!spec.quit) {
        uint32_t start_time = SDL_GetTicks();
        
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) spec.quit = true;
            else if (e.type == SDL_KEYDOWN) spectrum_handle_key(&spec, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP) spectrum_handle_key(&spec, e.key.keysym.scancode, false);
        }
        
        spectrum_run_frame(&spec);
        spectrum_render(&spec);
        
        uint32_t elapsed_time = SDL_GetTicks() - start_time;
        if (elapsed_time < frame_ticks) {
            SDL_Delay(frame_ticks - elapsed_time);
        }
    }
    
    spectrum_destroy(&spec);
    return 0;
}