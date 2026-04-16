/*
 * cpc.c  –  Amstrad CPC 6128 emulator core
 *
 * Implementa:
 *  · Z80A @ 4 MHz  (callbacks idénticos a minzx.c)
 *  · Gate Array (modos vídeo, paleta, banking, IRQ cada 52 líneas)
 *  · CRTC 6845 (registros de timing y posición de pantalla)
 *  · AY-3-8912 (3 canales tono + ruido + envolvente)
 *  · PPI 8255  (teclado matricial, motor cassette, altavoz)
 *  · Bancos de memoria 128 KB (configuración via puerto 0x7FFF)
 */

#include "cpc.h"
#include "cpc_fdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── Variables SDL (extern desde cpc_main.c) ──────────────────────────────────
extern SDL_Renderer*     cpc_renderer;
extern SDL_Texture*      cpc_texture;
extern SDL_AudioDeviceID cpc_audio_dev;

uint32_t cpc_pixels[CPC_H * CPC_W];

// ── Memoria ───────────────────────────────────────────────────────────────────
#define RAM_BANKS    8
#define BANK_SIZE    0x4000   // 16 KB
static uint8_t ram[RAM_BANKS][BANK_SIZE];
static uint8_t rom_os   [BANK_SIZE];   // OS ROM (lower)
static uint8_t rom_basic[BANK_SIZE];   // BASIC ROM (upper)
static uint8_t rom_amsdos[BANK_SIZE];  // AMSDOS ROM (upper bank 7)

// Configuración de banking activa (puerto 0x7FFF)
static uint8_t mem_config = 0x00;   // bits 0-2: RAM bank, bit 3: video page
static bool    rom_lo_en  = true;   // ROM OS en 0x0000-0x3FFF
static bool    rom_hi_en  = true;   // ROM BASIC en 0xC000-0xFFFF
static uint8_t ram_bank_hi = 0;     // banco de RAM en 0xC000-0xFFFF

// Página de vídeo (siempre banco 0 o banco 3)
static uint8_t* vram;    // apunta a ram[0] o ram[3]

// ── CPU ───────────────────────────────────────────────────────────────────────
static z80      cpu;
static uint32_t cpu_cycles = 0;

// ── FDC NEC µPD765A ──────────────────────────────────────────────────────────
static FDC fdc;

// ── CRTC 6845 ─────────────────────────────────────────────────────────────────
// Registros relevantes para emulación simplificada
static uint8_t crtc_reg[18];
static uint8_t crtc_sel = 0;
// Dirección base de la pantalla (calculada de R12/R13)
static uint16_t crtc_screen_addr = 0;

// ── Gate Array ────────────────────────────────────────────────────────────────
static uint8_t ga_pen    = 0;          // lápiz seleccionado (0-15, 16=borde)
static uint8_t ga_ink[17];            // color HW de cada lápiz (0-26)
static uint8_t ga_mode   = 1;         // modo vídeo (0,1,2)
static uint8_t ga_irq_counter = 0;    // IRQ cada 52 líneas
static bool    ga_irq_pending  = false;

int cpc_video_mode = 1;

// Paleta física del CPC (27 colores en ARGB)
static const uint32_t cpc_hw_palette[27] = {
    0xFF6E6E6E, // 0  White (en hardware es gris medio)
    0xFF6E6E6E, // 1  White
    0xFF00FF6E, // 2  Sea Green
    0xFFFFFF6E, // 3  Pastel Yellow
    0xFF000000, // 4  Black / Transparent (usado como negro)
    0xFFFF006E, // 5  Purple
    0xFF006E00, // 6  Green  (no Bright)
    0xFFFF6E00, // 7  Orange
    0xFF6E6EFF, // 8  Pastel Blue
    0xFFFFFFFF, // 9  Bright White
    0xFF00FFFF, // 10 Bright Cyan
    0xFFFFFFFF, // 11 Bright White
    0xFF0000FF, // 12 Bright Blue
    0xFFFF00FF, // 13 Bright Magenta
    0xFF00FF00, // 14 Bright Green
    0xFFFFFF00, // 15 Bright Yellow
    0xFF000000, // 16 Black
    0xFFFFFF6E, // 17 Pastel Yellow
    0xFF00FF6E, // 18 Sea Green
    0xFFFFFF6E, // 19 Pastel Yellow
    0xFF000000, // 20 Black
    0xFFFF006E, // 21 Purple
    0xFF006E00, // 22 Dark Green
    0xFFFF6E00, // 23 Orange
    0xFF6E6EFF, // 24 Pastel Blue
    0xFFFFFFFF, // 25 White
    0xFF00FFFF, // 26 Pastel Cyan
};

// ── AY-3-8912 ─────────────────────────────────────────────────────────────────
static uint8_t ay_reg[16];
static uint8_t ay_sel = 0;

typedef struct {
    double   freq;
    double   phase;
    uint16_t period;
    uint16_t counter;
    bool     tone_en;
    bool     noise_en;
    uint8_t  vol;
    bool     env_en;
} AYChannel;

static AYChannel ay_ch[3];
static uint16_t ay_noise_period;
static uint16_t ay_noise_counter;
static uint32_t ay_noise_lfsr = 1;
static uint8_t  ay_noise_out  = 0;
static uint16_t ay_env_period;
static uint16_t ay_env_counter;
static uint8_t  ay_env_shape;
static uint8_t  ay_env_vol;
static bool     ay_env_hold;
static bool     ay_env_alt;
static bool     ay_env_att;
static bool     ay_env_cont;

static const int16_t AY_VOL_TABLE[16] = {
    0, 170, 240, 340, 480, 680, 960, 1360,
    1920, 2720, 3840, 5440, 7680, 10880, 15360, 21760
};

static void ay_write(uint8_t reg, uint8_t val) {
    ay_reg[reg & 0x0F] = val;
    // Actualizar parámetros de canales
    for (int c = 0; c < 3; c++) {
        uint16_t p = ((uint16_t)(ay_reg[c*2+1] & 0x0F) << 8) | ay_reg[c*2];
        ay_ch[c].period  = p ? p : 1;
        ay_ch[c].tone_en = !((ay_reg[7] >> c) & 1);
        ay_ch[c].noise_en= !((ay_reg[7] >> (c+3)) & 1);
        ay_ch[c].vol     = ay_reg[8+c] & 0x0F;
        ay_ch[c].env_en  = (ay_reg[8+c] & 0x10) != 0;
    }
    ay_noise_period  = ay_reg[6] & 0x1F;
    if (!ay_noise_period) ay_noise_period = 1;
    ay_env_period    = ((uint16_t)ay_reg[12] << 8) | ay_reg[11];
    if (!ay_env_period) ay_env_period = 1;
    if (reg == 13) {
        // Reset envelope
        ay_env_counter = 0;
        ay_env_vol     = (ay_reg[13] & 4) ? 15 : 0;
        ay_env_att     = (ay_reg[13] & 4) != 0;
        ay_env_alt     = (ay_reg[13] & 2) != 0;
        ay_env_hold    = (ay_reg[13] & 1) != 0;
        ay_env_cont    = (ay_reg[13] & 8) != 0;
    }
}

static int16_t ay_sample(void) {
    // Avanzar contadores de ruido
    if (++ay_noise_counter >= ay_noise_period * 8) {
        ay_noise_counter = 0;
        if ((ay_noise_lfsr & 1) ^ ((ay_noise_lfsr >> 3) & 1))
            ay_noise_lfsr = (ay_noise_lfsr >> 1) | 0x10000;
        else
            ay_noise_lfsr >>= 1;
        ay_noise_out = ay_noise_lfsr & 1;
    }
    // Avanzar envolvente
    if (++ay_env_counter >= ay_env_period * 8) {
        ay_env_counter = 0;
        if (!ay_env_hold) {
            if (ay_env_att) { if (ay_env_vol < 15) ay_env_vol++; else { if (!ay_env_cont) ay_env_hold=true; if (ay_env_alt) ay_env_att=!ay_env_att; } }
            else            { if (ay_env_vol > 0)  ay_env_vol--; else { if (!ay_env_cont) ay_env_hold=true; if (ay_env_alt) ay_env_att=!ay_env_att; } }
        }
    }

    int32_t mix = 0;
    for (int c = 0; c < 3; c++) {
        // Avanzar tono
        if (++ay_ch[c].counter >= ay_ch[c].period * 8) {
            ay_ch[c].counter = 0;
            ay_ch[c].phase   = 1.0 - ay_ch[c].phase;
        }
        int out = (ay_ch[c].phase >= 0.5 ? 1 : 0);
        if (!ay_ch[c].tone_en  || out)
        if (!ay_ch[c].noise_en || ay_noise_out) {
            int16_t vol = ay_ch[c].env_en ?
                          AY_VOL_TABLE[ay_env_vol] :
                          AY_VOL_TABLE[ay_ch[c].vol];
            mix += vol;
        }
    }
    mix /= 3;
    if (mix >  INT16_MAX) mix =  INT16_MAX;
    if (mix <  INT16_MIN) mix =  INT16_MIN;
    return (int16_t)mix;
}

// ── PPI 8255 ──────────────────────────────────────────────────────────────────
static uint8_t ppi_portA = 0xFF;  // AY data bus
static uint8_t ppi_portB = 0xFF;  // keyboard row, vsync, etc.
static uint8_t ppi_portC = 0x00;  // AY ctrl, keyboard column, motor, speaker
static uint8_t ppi_ctrl  = 0x82;

uint8_t cpc_keymap[10];  // 10 filas, bit=0 → pulsada

// ── Audio buffer ──────────────────────────────────────────────────────────────
static int16_t audio_buf[512];
static size_t  audio_buf_len = 0;
static double  audio_next_t  = 0.0;
static const double CPC_CYC_PER_SAMPLE =
    (double)CPC_CLOCK_HZ / (double)CPC_AUDIO_RATE;

static void audio_push(int16_t s) {
    audio_buf[audio_buf_len++] = s;
    if (audio_buf_len == sizeof(audio_buf)/sizeof(audio_buf[0])) {
        SDL_QueueAudio(cpc_audio_dev, audio_buf,
                       (Uint32)(audio_buf_len * sizeof(int16_t)));
        audio_buf_len = 0;
    }
}
static void audio_flush(void) {
    if (audio_buf_len) {
        SDL_QueueAudio(cpc_audio_dev, audio_buf,
                       (Uint32)(audio_buf_len * sizeof(int16_t)));
        audio_buf_len = 0;
    }
}

// Avanza cpu_cycles y genera audio
static void addCycles(uint32_t delta) {
    if (!delta) return;
    double end = (double)cpu_cycles + delta;
    while ((double)cpu_cycles < end) {
        if (audio_next_t <= (double)cpu_cycles) {
            audio_push(ay_sample());
            audio_next_t += CPC_CYC_PER_SAMPLE;
        }
        double gap = audio_next_t - (double)cpu_cycles;
        double left= end - (double)cpu_cycles;
        uint32_t step = (uint32_t)(gap < left ? gap : left);
        if (!step) step = 1;
        cpu_cycles += step;
    }
}

// ── Bus de memoria ────────────────────────────────────────────────────────────
static uint8_t mem_read_cb(void* ud, uint16_t addr) {
    addCycles(3);
    if (addr < 0x4000) {
        if (rom_lo_en) return rom_os[addr];
        return ram[0][addr];
    }
    if (addr < 0x8000) return ram[1][addr - 0x4000];
    if (addr < 0xC000) return ram[2][addr - 0x8000];
    // 0xC000-0xFFFF
    if (rom_hi_en) return rom_basic[addr - 0xC000];
    return ram[ram_bank_hi][addr - 0xC000];
}

static void mem_write_cb(void* ud, uint16_t addr, uint8_t val) {
    addCycles(3);
    if      (addr < 0x4000) ram[0][addr]          = val;
    else if (addr < 0x8000) ram[1][addr - 0x4000] = val;
    else if (addr < 0xC000) ram[2][addr - 0x8000] = val;
    else                    ram[ram_bank_hi][addr - 0xC000] = val;
}

// ── Bus de I/O ────────────────────────────────────────────────────────────────
static uint8_t io_read_cb(z80* z, uint16_t port) {
    addCycles(4);
    uint8_t hi = port >> 8;
    uint8_t lo = port & 0xFF;

    // CRTC: A14=0, A9=1 → puerto 0xBCxx / 0xBDxx
    if (!(hi & 0x40) && (hi & 0x80)) {
        if (!(lo & 0x01)) return 0xFF;   // write-only select
        return crtc_reg[crtc_sel];       // status (simplificado)
    }

    // FDC: puerto 0xFB7F (A10=0, A7=1, A1=1, A0=1)
    // Status register: A8=0 → 0xFB7E (motor), A8=1 → 0xFB7F (status/data)
    if ((hi & 0xFE) == 0xFA) {
        // 0xFA7E / 0xFB7E = motor control port (write only, read returns FF)
        return 0xFF;
    }
    if (hi == 0xFB) {
        if (lo == 0x7F) return fdc_read_status(&fdc);   // MSR
        if (lo == 0xFF) return fdc_read_data(&fdc);     // data reg
        return 0xFF;
    }

    // PPI: A11=0 → 0xF4xx-0xF7xx
    if (!(hi & 0x08) && (hi & 0xF0) == 0xF0) {
        switch (lo & 0x03) {
            case 0: {
                // Puerto A: AY data / AY no activo
                uint8_t ay_ctrl = (ppi_portC >> 6) & 3;
                if (ay_ctrl == 1) return ay_reg[ay_sel];
                return ppi_portA;
            }
            case 1: {
                // Puerto B: bit7=vsync, bits 0-3=fila teclado
                uint8_t row = (ppi_portC & 0x0F) < 10 ?
                              cpc_keymap[ppi_portC & 0x0F] : 0xFF;
                return (row & 0x3F) | 0x40;  // bit6=cassette in, bit7=vsync(simplif)
            }
            case 2: return ppi_portC;
        }
    }
    return 0xFF;
}

static void io_write_cb(z80* z, uint16_t port, uint8_t val) {
    addCycles(4);
    uint8_t hi = port >> 8;
    uint8_t lo = port & 0xFF;

    // Gate Array: A15=1,A14=0 → 0x7Fxx
    if ((hi & 0xC0) == 0x40) {
        switch (val >> 6) {
            case 0:  // seleccionar lápiz
                ga_pen = val & 0x1F;
                break;
            case 1:  // asignar color al lápiz
                ga_ink[ga_pen & 0x1F] = val & 0x1F;
                break;
            case 2:  // modo vídeo + ROM enable + IRQ reset
                ga_mode = val & 3;
                cpc_video_mode = ga_mode;
                rom_lo_en = !((val >> 2) & 1);
                rom_hi_en = !((val >> 3) & 1);
                if (val & 0x10) { ga_irq_counter = 0; ga_irq_pending = false; }
                break;
            case 3:  // banking RAM (CPC 6128)
                if (val & 0x04) {
                    // bits 0-2: RAM bank extra para 0xC000-0xFFFF
                    uint8_t bank = val & 0x07;
                    ram_bank_hi = (bank < RAM_BANKS) ? bank : 0;
                }
                break;
        }
        return;
    }

    // CRTC: A15=1,A14=0,A9=0 → 0xBC/0xBD
    if (!(hi & 0x40) && (hi & 0x80)) {
        if (!(lo & 0x01)) crtc_sel = val & 0x1F;
        else {
            if (crtc_sel < 18) crtc_reg[crtc_sel] = val;
            if (crtc_sel == 12 || crtc_sel == 13)
                crtc_screen_addr = ((uint16_t)(crtc_reg[12] & 0x3F) << 8)
                                 | crtc_reg[13];
        }
        return;
    }

    // FDC motor control: puerto 0xFA7E / 0xFB7E
    if ((hi & 0xFE) == 0xFA && (lo == 0x7E || lo == 0xFE)) {
        fdc_motor_control(&fdc, val);
        return;
    }
    // FDC data register: puerto 0xFB7F
    if (hi == 0xFB && (lo == 0x7F || lo == 0xFF)) {
        fdc_write_data(&fdc, val);
        return;
    }

    // PPI: A11=0
    if (!(hi & 0x08) && (hi & 0xF0) == 0xF0) {
        uint8_t ppireg = lo & 0x03;
        if (ppireg == 3 && (val & 0x80)) {
            ppi_ctrl = val;
        } else switch (ppireg) {
            case 0: {
                ppi_portA = val;
                // AY bus write
                uint8_t ctrl = (ppi_portC >> 6) & 3;
                if      (ctrl == 3) { ay_sel = val & 0x0F; }
                else if (ctrl == 2) { ay_write(ay_sel, val); }
                break;
            }
            case 1: ppi_portB = val; break;
            case 2:
                ppi_portC = val;
                {
                    uint8_t ctrl = (val >> 6) & 3;
                    if (ctrl == 3) { ay_sel = ppi_portA & 0x0F; }
                    else if (ctrl == 2) { ay_write(ay_sel, ppi_portA); }
                }
                break;
        }
        return;
    }
}

// ── Renderizado ───────────────────────────────────────────────────────────────
// El CPC renderiza en modo 1 (320×200) por defecto.
// vram en 0xC000: cada byte = 2 pixels en modo 1, 4 en modo 0, 8 en modo 2.

static void render_frame(void) {
    memset(cpc_pixels, 0, sizeof(cpc_pixels));

    // Dirección inicial de vídeo (CRTC R12/R13, cada unidad = 2 bytes)
    uint16_t addr = (crtc_screen_addr & 0x3FF) << 1;

    // Colores activos
    uint32_t ink[16];
    for (int i = 0; i < 16; i++)
        ink[i] = cpc_hw_palette[ga_ink[i] & 0x1A];
    uint32_t border = cpc_hw_palette[ga_ink[16] & 0x1A];

    int lines    = 200;
    int cols     = 80;     // bytes por línea (modo 1: 80 bytes = 160 dobles px)
    int pix_w    = (ga_mode == 0) ? 4 : (ga_mode == 1) ? 2 : 1;
    int scr_cols = (ga_mode == 0) ? 160 : (ga_mode == 1) ? 320 : 640;
    int x_off    = (CPC_W - scr_cols) / 2;
    int y_off    = (CPC_H - lines) / 2;

    for (int y = 0; y < lines; y++) {
        // El CPC tiene un layout de líneas interleaved:
        // fila física = (y & 7) << 11 | (y >> 3) * 80 + base
        uint16_t line_addr = addr + ((y & 7) << 11) + (y >> 3) * cols;
        uint32_t* row = cpc_pixels + (y + y_off) * CPC_W;

        // Borde izquierdo
        for (int x = 0; x < x_off; x++) row[x] = border;

        for (int b = 0; b < cols; b++) {
            uint8_t byte = vram[(line_addr + b) & 0x3FFF];
            int scr_x = x_off + b * pix_w * (ga_mode == 2 ? 1 : 1) * 8 / (8 / pix_w);

            switch (ga_mode) {
                case 0: // 160×200, 4 bits/pixel, 2 px/byte
                    for (int p = 0; p < 2; p++) {
                        // Bits: pixel0=[7,5,3,1], pixel1=[6,4,2,0]
                        int shift = p ? 0 : 1;
                        int ci = ((byte >> (7-shift)) & 1) << 3 |
                                 ((byte >> (5-shift)) & 1) << 2 |
                                 ((byte >> (3-shift)) & 1) << 1 |
                                 ((byte >> (1-shift)) & 1);
                        int px = scr_x + p * 2;
                        if (px < CPC_W && px+1 < CPC_W) {
                            row[px] = row[px+1] = ink[ci & 0xF];
                        }
                    }
                    break;
                case 1: // 320×200, 2 bits/pixel, 4 px/byte
                    for (int p = 0; p < 4; p++) {
                        int ci = ((byte >> (7-p)) & 1) << 1 |
                                 ((byte >> (3-p+(ga_mode==1?4:0))) & 1); // simplif
                        // Bit layout modo 1: p0=[7,3], p1=[6,2], p2=[5,1], p3=[4,0]
                        ci = ((byte >> (7-p)) & 1) << 1 | ((byte >> (3-p)) & 1);
                        int px = scr_x + p;
                        if (px < CPC_W) row[px] = ink[ci & 3];
                    }
                    break;
                case 2: // 640×200, 1 bit/pixel, 8 px/byte
                    for (int p = 0; p < 8; p++) {
                        int ci = (byte >> (7-p)) & 1;
                        int px = (x_off/2) + b * 8 + p;
                        if (px < CPC_W) row[px] = ink[ci];
                    }
                    break;
            }
        }
        // Borde derecho
        int edge = x_off + scr_cols;
        for (int x = edge; x < CPC_W; x++) row[x] = border;
    }
    // Bordes superior e inferior
    for (int y = 0; y < y_off; y++)
        for (int x = 0; x < CPC_W; x++) cpc_pixels[y*CPC_W+x] = border;
    for (int y = y_off+lines; y < CPC_H; y++)
        for (int x = 0; x < CPC_W; x++) cpc_pixels[y*CPC_W+x] = border;
}

void cpc_render(void) {
    SDL_UpdateTexture(cpc_texture, NULL, cpc_pixels,
                      CPC_W * sizeof(uint32_t));
    SDL_RenderClear(cpc_renderer);
    SDL_RenderCopy(cpc_renderer, cpc_texture, NULL, NULL);
    SDL_RenderPresent(cpc_renderer);
}

// ── Frame update ──────────────────────────────────────────────────────────────
void cpc_update(void) {
    const uint32_t frame_start = cpu_cycles;
    const uint32_t frame_end   = frame_start + CPC_CYCLES_FRAME;

    // IRQ del Gate Array cada 52 líneas (~ cada 52×64 = 3328 ciclos)
    uint32_t next_irq = frame_start + 3328;

    while (cpu_cycles < frame_end) {
        int cyc = z80_step(&cpu);
        if (cyc <= 0) cyc = 4;
        addCycles((uint32_t)cyc);

        // Gate Array genera IRQ cada 52 líneas de pantalla
        if (cpu_cycles >= next_irq && next_irq < frame_end) {
            ga_irq_counter++;
            if (ga_irq_counter >= 52) {
                ga_irq_counter = 0;
                ga_irq_pending = true;
            }
            next_irq += 3328;
        }

        if (ga_irq_pending) {
            ga_irq_pending = false;
            z80_pulse_irq(&cpu, 0xFF);   // modo 1: vector fijo 0x38
        }

        // FDC tick: actualizar estado interno del controlador de disco
        fdc_tick(&fdc, cyc);
    }

    render_frame();
    audio_flush();
}

// ── Reset ─────────────────────────────────────────────────────────────────────
void cpc_reset(void) {
    z80_init(&cpu);
    cpu.userdata   = NULL;
    cpu.read_byte  = mem_read_cb;
    cpu.write_byte = mem_write_cb;
    cpu.port_in    = io_read_cb;
    cpu.port_out   = io_write_cb;
    rom_lo_en   = true;
    rom_hi_en   = true;
    ram_bank_hi = 0;
    ga_mode     = 1;
    ga_pen      = 0;
    ga_irq_counter = 0;
    ga_irq_pending = false;
    memset(ga_ink,   0, sizeof(ga_ink));
    memset(ay_reg,   0, sizeof(ay_reg));
    memset(crtc_reg, 0, sizeof(crtc_reg));
    // Colores iniciales del firmware CPC
    static const uint8_t default_ink[17] = {
        1,24,20,6,26,0,2,8,10,12,14,16,18,22,24,16,6
    };
    memcpy(ga_ink, default_ink, 17);
    fdc_reset(&fdc);
    printf("CPC 6128: RESET\n");
}

// ── Init ──────────────────────────────────────────────────────────────────────
static int load_file(const char* p, uint8_t* buf, size_t sz) {
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    return (rd == sz) ? 0 : -1;
}

int cpc_init(const char* rom_dir) {
    char path[512];
    // Intentar ROM unificada de 48 KB
    snprintf(path, sizeof(path), "%s/cpc6128.rom", rom_dir);
    uint8_t big_rom[0xC000];
    if (load_file(path, big_rom, 0xC000) == 0) {
        memcpy(rom_os,    big_rom,          BANK_SIZE);
        memcpy(rom_basic, big_rom+BANK_SIZE, BANK_SIZE);
        memcpy(rom_amsdos,big_rom+BANK_SIZE*2, BANK_SIZE);
    } else {
        // ROMs separadas
        snprintf(path,sizeof(path),"%s/os.rom",rom_dir);
        if (load_file(path,rom_os,BANK_SIZE)<0) {
            fprintf(stderr,"CPC: os.rom no encontrada en %s\n",rom_dir); return -1; }
        snprintf(path,sizeof(path),"%s/basic.rom",rom_dir);
        if (load_file(path,rom_basic,BANK_SIZE)<0) {
            fprintf(stderr,"CPC: basic.rom no encontrada\n"); return -1; }
        snprintf(path,sizeof(path),"%s/amsdos.rom",rom_dir);
        load_file(path,rom_amsdos,BANK_SIZE); // opcional
    }

    memset(ram, 0, sizeof(ram));
    vram = ram[0];   // vídeo por defecto en banco 0
    memset(cpc_keymap, 0xFF, sizeof(cpc_keymap));

    cpu_cycles     = 0;
    audio_next_t   = 0.0;
    audio_buf_len  = 0;

    fdc_init(&fdc);
    cpc_reset();
    printf("CPC 6128: %d Hz, %dx%d, AY-3-8912\n",
           CPC_CLOCK_HZ, CPC_W, CPC_H);
    return 0;
}

void cpc_quit(void) {}

bool cpc_load_sna(const char* path) {
    // Formato .SNA del CPC (256 bytes de cabecera + 64KB RAM)
    FILE* f = fopen(path,"rb");
    if (!f) return false;
    uint8_t hdr[256];
    if (fread(hdr,1,256,f)!=256) { fclose(f); return false; }
    // Registros Z80 en el header (offset documentado)
    cpu.af  = ((uint16_t)hdr[0x11]<<8)|hdr[0x10];
    cpu.bc  = ((uint16_t)hdr[0x13]<<8)|hdr[0x12];
    cpu.de  = ((uint16_t)hdr[0x15]<<8)|hdr[0x14];
    cpu.hl  = ((uint16_t)hdr[0x17]<<8)|hdr[0x16];
    cpu.a_f_= ((uint16_t)hdr[0x1B]<<8)|hdr[0x1A];
    cpu.b_c_= ((uint16_t)hdr[0x1D]<<8)|hdr[0x1C];
    cpu.d_e_= ((uint16_t)hdr[0x1F]<<8)|hdr[0x1E];
    cpu.h_l_= ((uint16_t)hdr[0x21]<<8)|hdr[0x20];
    cpu.ix  = ((uint16_t)hdr[0x23]<<8)|hdr[0x22];
    cpu.iy  = ((uint16_t)hdr[0x25]<<8)|hdr[0x24];
    cpu.sp  = ((uint16_t)hdr[0x27]<<8)|hdr[0x26];
    cpu.pc  = ((uint16_t)hdr[0x29]<<8)|hdr[0x28];
    cpu.interrupt_mode = hdr[0x33];
    cpu.iff1= hdr[0x34] & 1;
    cpu.iff2= hdr[0x35] & 1;
    ga_mode = hdr[0x6D] & 3;
    uint32_t total_ram = ((uint32_t)hdr[0x6B]<<8)|hdr[0x6A];
    for (uint32_t b=0; b*16384<total_ram && b<RAM_BANKS; b++)
        fread(ram[b],1,16384,f);
    fclose(f);
    printf("CPC SNA cargado: %s PC=0x%04X\n", path, cpu.pc);
    return true;
}

bool cpc_load_dsk(const char* path) {
    // Cargar DSK en la unidad A: por defecto
    return fdc_load_dsk(&fdc, 0, path);
}

bool cpc_load_dsk_drive(const char* path, int drive) {
    return fdc_load_dsk(&fdc, drive, path);
}

void cpc_eject_disk(int drive) {
    fdc_eject(&fdc, drive);
}

bool cpc_disk_inserted(int drive) {
    if (drive < 0 || drive >= FDC_MAX_DRIVES) return false;
    return fdc.drives[drive].disk.inserted;
}
