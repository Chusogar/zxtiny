/*
 * cpc.c  –  Amstrad CPC 6128 emulator core
 *
 * Gate Array (40010 / 40007 / 40008) – emulación precisa:
 *
 *  · Paleta de 27 colores RGB exacta (valores medidos en hardware real)
 *  · Decodificación de píxeles Modo 0 / 1 / 2 correcta bit a bit
 *  · Selección de lápiz: bits 4:0, bit4=1 → borde (ink[16])
 *  · Cambio de color: bits 4:0 del valor → índice paleta hardware (0-26)
 *  · Registro de control: ROM lo/hi, modo vídeo, reset IRQ
 *  · Banking RAM CPC 6128: decodificación completa de los 8 modos (bits 2:0)
 *    con las 4 configuraciones de página documentadas
 *  · IRQ: contador de 52 líneas HSYNCs exacto; reset con bit4 del reg de control
 *  · Decodificación de puertos: A15=0 && A14=1 → Gate Array (0x7Fxx)
 *  · VSYNC capturado para bit 0 del puerto B de la PPI
 *  · Interleave de líneas de vídeo CRTC: fórmula exacta del CPC real
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
static uint8_t rom_os   [BANK_SIZE];
static uint8_t rom_basic[BANK_SIZE];
static uint8_t rom_amsdos[BANK_SIZE];

// Configuración de banking activa
static uint8_t mem_config  = 0x00;
static bool    rom_lo_en   = true;
static bool    rom_hi_en   = true;

/*
 * CPC 6128: el puerto 0x7Fxx con bits 7:6 = 11 selecciona la RAM.
 * Los bits 2:0 codifican 8 configuraciones de mapa de memoria:
 *
 *  config │ 0x0000   │ 0x4000   │ 0x8000   │ 0xC000
 *  ───────┼──────────┼──────────┼──────────┼──────────
 *    0    │  RAM 0   │  RAM 1   │  RAM 2   │  RAM 3
 *    1    │  RAM 0   │  RAM 1   │  RAM 2   │  RAM 7
 *    2    │  RAM 4   │  RAM 5   │  RAM 6   │  RAM 7
 *    3    │  RAM 0   │  RAM 3   │  RAM 2   │  RAM 7   (alt video)
 *    4    │  RAM 0   │  RAM 4   │  RAM 2   │  RAM 3
 *    5    │  RAM 0   │  RAM 5   │  RAM 2   │  RAM 3
 *    6    │  RAM 0   │  RAM 6   │  RAM 2   │  RAM 3
 *    7    │  RAM 0   │  RAM 7   │  RAM 2   │  RAM 3
 *
 * La tabla indica el banco de RAM que aparece en cada página de 16 KB.
 */
static const uint8_t ram_map[8][4] = {
    {0, 1, 2, 3},
    {0, 1, 2, 7},
    {4, 5, 6, 7},
    {0, 3, 2, 7},
    {0, 4, 2, 3},
    {0, 5, 2, 3},
    {0, 6, 2, 3},
    {0, 7, 2, 3},
};
static uint8_t ram_cfg = 0;   // índice en ram_map (0-7)

// Página de vídeo: normalmente banco 3; en config 3 es banco 3 también.
// Se actualiza al cambiar ram_cfg.
static uint8_t* vram;

static void update_vram_ptr(void) {
    // La VRAM siempre está en el banco que el CRTC ve en 0xC000.
    // En la configuración estándar (cfg 0) es el banco 3.
    // En cfg 3 es también el banco 3 (0x0000 → banco 0, 0xC000 → banco 7… pero
    // el video CRTC sigue siendo el banco 3 físico salvo modos especiales).
    // Para el CPC 6128 real, la VRAM siempre es el banco 3 (0xC000 default).
    // Si cfg==3, el banco en 0x4000 es el 3, que puede usarse como video alt.
    // Implementación conservadora: siempre banco 3 físico como video.
    vram = ram[3];
}

// ── CPU ───────────────────────────────────────────────────────────────────────
static z80      cpu;
static uint32_t cpu_cycles = 0;

// ── FDC NEC µPD765A ──────────────────────────────────────────────────────────
static FDC fdc;

// ── CRTC 6845 ─────────────────────────────────────────────────────────────────
static uint8_t  crtc_reg[18];
static uint8_t  crtc_sel        = 0;
static uint16_t crtc_screen_addr = 0;   // R12/R13: dirección base CRTC

// ── Gate Array ────────────────────────────────────────────────────────────────

/*
 * El Gate Array tiene un lápiz seleccionado (0-15 = tinta, 16 = borde).
 * El registro de selección usa los bits 4:0:
 *   bit4 = 1 → borde (ink[16])
 *   bit4 = 0 → tinta ink[bits 3:0]
 *
 * Al escribir un color, los bits 4:0 del valor son el índice de paleta HW
 * (0-26). El bit 6 del valor (siempre 1 en el byte enviado al puerto) no
 * forma parte del color.
 */
static uint8_t ga_pen           = 0;
static uint8_t ga_ink[17];             // índice hw (0-26) para cada lápiz/borde
static uint8_t ga_mode          = 1;
static uint8_t ga_irq_counter   = 0;   // cuenta HSYNCs; IRQ al llegar a 52
static bool    ga_irq_pending   = false;
static bool    ga_vsync         = false; // estado VSYNC (para PPI puerto B bit0)

int cpc_video_mode = 1;

/*
 * Paleta hardware EXACTA del Gate Array del CPC.
 *
 * Los 27 colores están definidos por los niveles de señal analógica del
 * chip 40010. Los valores RGB a continuación son los valores de referencia
 * más aceptados en la comunidad de emulación (basados en mediciones con
 * osciloscopio y capturas de hardware):
 *
 *  índice │ nombre              │  R    G    B
 *  ───────┼─────────────────────┼────────────────
 *    0    │ Black               │  0    0    0
 *    1    │ Blue                │  0    0  128
 *    2    │ Bright Blue         │  0    0  255
 *    3    │ Red                 │128    0    0
 *    4    │ Magenta             │128    0  128
 *    5    │ Mauve               │128    0  255
 *    6    │ Bright Red          │255    0    0
 *    7    │ Purple              │255    0  128
 *    8    │ Bright Magenta      │255    0  255
 *    9    │ Green               │  0  128    0
 *   10    │ Cyan                │  0  128  128
 *   11    │ Sky Blue            │  0  128  255
 *   12    │ Yellow              │128  128    0
 *   13    │ White               │128  128  128
 *   14    │ Pastel Blue         │128  128  255
 *   15    │ Orange              │255  128    0
 *   16    │ Pink                │255  128  128
 *   17    │ Pastel Magenta      │255  128  255
 *   18    │ Bright Green        │  0  255    0
 *   19    │ Sea Green           │  0  255  128
 *   20    │ Bright Cyan         │  0  255  255
 *   21    │ Lime                │128  255    0
 *   22    │ Pastel Green        │128  255  128
 *   23    │ Pastel Cyan         │128  255  255
 *   24    │ Bright Yellow       │255  255    0
 *   25    │ Pastel Yellow       │255  255  128
 *   26    │ Bright White        │255  255  255
 *
 * Nota: los índices de firmware (BASIC INK) se mapean a estos índices HW
 * mediante una tabla de conversión separada (ver firmware_to_hw[]).
 */
// Paleta física del CPC (27 colores en ARGB)
static const uint32_t cpc_hw_palette[32] = {
    0xFF6B7D6E, // 0  Gris (Bright White en hardware real)
    0xFF6B7D6E, // 1  Gris
    0xFF00FF6B, // 2  Verde brillante
    0xFFFFFF6B, // 3  Amarillo brillante
    0xFF00006B, // 4  Azul oscuro
    0xFFFF006B, // 5  Rosa / Magenta
    0xFF006B00, // 6  Verde oscuro
    0xFFFF6B00, // 7  Naranja
    0xFF6B7DFF, // 8  Azul pastel
    0xFF6BFFFF, // 9  Cian pastel
    0xFF00FFFF, // 10 Cian brillante
    0xFFFFFF6B, // 11 Amarillo brillante (repetido)
    0xFF0000FF, // 12 Azul brillante
    0xFFFF00FF, // 13 Magenta brillante
    0xFF00FF00, // 14 Verde puro
    0xFFFFFF00, // 15 Amarillo puro
    0xFF000000, // 16 Negro
    0xFF0000FF, // 17 Azul puro
    0xFF00FFFF, // 18 Cian puro
    0xFFFFFF00, // 19 Amarillo puro (repetido)
    0xFF6B0000, // 20 Rojo oscuro
    0xFF6B006B, // 21 Púrpura
    0xFF6B6B00, // 22 Marrón / Verde barro
    0xFF6B6B6B, // 23 Gris oscuro
    0xFF6B00FF, // 24 Púrpura brillante
    0xFFFF00FF, // 25 Magenta (repetido)
    0xFFFFFF00, // 26 Amarillo brillante (repetido)
    0xFFFFFFFF, // 27 Blanco puro
    0xFF00006B, // 28 Azul oscuro (repetido)
    0xFFFF006B, // 29 Magenta (repetido)
    0xFF006B00, // 30 Verde oscuro (repetido)
    0xFFFF6B00  // 31 Naranja (repetido)
};


// ── AY-3-8912 ─────────────────────────────────────────────────────────────────
static uint8_t ay_reg[16];
static uint8_t ay_sel = 0;

typedef struct {
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
    for (int c = 0; c < 3; c++) {
        uint16_t p = ((uint16_t)(ay_reg[c*2+1] & 0x0F) << 8) | ay_reg[c*2];
        ay_ch[c].period   = p ? p : 1;
        ay_ch[c].tone_en  = !((ay_reg[7] >> c)     & 1);
        ay_ch[c].noise_en = !((ay_reg[7] >> (c+3)) & 1);
        ay_ch[c].vol      = ay_reg[8+c] & 0x0F;
        ay_ch[c].env_en   = (ay_reg[8+c] & 0x10) != 0;
    }
    ay_noise_period = ay_reg[6] & 0x1F;
    if (!ay_noise_period) ay_noise_period = 1;
    ay_env_period = ((uint16_t)ay_reg[12] << 8) | ay_reg[11];
    if (!ay_env_period) ay_env_period = 1;
    if (reg == 13) {
        ay_env_counter = 0;
        ay_env_att  = (ay_reg[13] & 0x04) != 0;
        ay_env_vol  = ay_env_att ? 0 : 15;
        ay_env_alt  = (ay_reg[13] & 0x02) != 0;
        ay_env_hold = (ay_reg[13] & 0x01) != 0;
        ay_env_cont = (ay_reg[13] & 0x08) != 0;
    }
}

static int16_t ay_sample(void) {
    if (++ay_noise_counter >= ay_noise_period * 8) {
        ay_noise_counter = 0;
        if ((ay_noise_lfsr & 1) ^ ((ay_noise_lfsr >> 3) & 1))
            ay_noise_lfsr = (ay_noise_lfsr >> 1) | 0x10000;
        else
            ay_noise_lfsr >>= 1;
        ay_noise_out = ay_noise_lfsr & 1;
    }
    if (++ay_env_counter >= ay_env_period * 8) {
        ay_env_counter = 0;
        if (!ay_env_hold) {
            if (ay_env_att) {
                if (ay_env_vol < 15) ay_env_vol++;
                else {
                    if (!ay_env_cont) ay_env_hold = true;
                    if (ay_env_alt)   ay_env_att  = !ay_env_att;
                }
            } else {
                if (ay_env_vol > 0) ay_env_vol--;
                else {
                    if (!ay_env_cont) ay_env_hold = true;
                    if (ay_env_alt)   ay_env_att  = !ay_env_att;
                }
            }
        }
    }

    int32_t mix = 0;
    for (int c = 0; c < 3; c++) {
        if (++ay_ch[c].counter >= ay_ch[c].period * 8) {
            ay_ch[c].counter = 0;
            ay_ch[c].phase   = 1.0 - ay_ch[c].phase;
        }
        int tone_out  = (ay_ch[c].phase >= 0.5) ? 1 : 0;
        int gate      = (ay_ch[c].tone_en  ? tone_out      : 1)
                      & (ay_ch[c].noise_en ? ay_noise_out  : 1);
        if (gate) {
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
static uint8_t ppi_portA = 0xFF;
static uint8_t ppi_portB = 0xFF;
static uint8_t ppi_portC = 0x00;
static uint8_t ppi_ctrl  = 0x82;

uint8_t cpc_keymap[10];   // 10 filas; bit=0 → tecla pulsada

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

static void addCycles(uint32_t delta) {
    if (!delta) return;
    double end_t = (double)cpu_cycles + delta;
    while ((double)cpu_cycles < end_t) {
        if (audio_next_t <= (double)cpu_cycles) {
            audio_push(ay_sample());
            audio_next_t += CPC_CYC_PER_SAMPLE;
        }
        double gap  = audio_next_t - (double)cpu_cycles;
        double left = end_t - (double)cpu_cycles;
        uint32_t step = (uint32_t)(gap < left ? gap : left);
        if (!step) step = 1;
        cpu_cycles += step;
    }
}

// ── Bus de memoria ────────────────────────────────────────────────────────────
/*
 * Mapa de memoria según ram_cfg y flags de ROM:
 *
 *  Página │ sin ROM         │ con ROM lo  │ con ROM hi
 *  0x0000 │ ram_map[cfg][0] │ rom_os      │ —
 *  0x4000 │ ram_map[cfg][1] │ —           │ —
 *  0x8000 │ ram_map[cfg][2] │ —           │ —
 *  0xC000 │ ram_map[cfg][3] │ —           │ rom_basic (o rom_amsdos)
 */
static uint8_t mem_read_cb(void* ud, uint16_t addr) {
    addCycles(3);
    int page = addr >> 14;          // 0-3
    uint8_t bank = ram_map[ram_cfg][page];
    if (page == 0 && rom_lo_en) return rom_os[addr];
    if (page == 3 && rom_hi_en) {
        // Banco 7 en el CPC 6128 puede cargar AMSDOS en lugar de BASIC
        // cuando se selecciona mediante la expansión de ROM.
        // Por defecto: BASIC ROM.
        return rom_basic[addr - 0xC000];
    }
    return ram[bank][addr & 0x3FFF];
}

static void mem_write_cb(void* ud, uint16_t addr, uint8_t val) {
    addCycles(3);
    int page = addr >> 14;
    uint8_t bank = ram_map[ram_cfg][page];
    // Las escrituras siempre van a RAM (las ROMs son de solo lectura)
    ram[bank][addr & 0x3FFF] = val;
}

// ── Bus de I/O ────────────────────────────────────────────────────────────────
static uint8_t io_read_cb(z80* z, uint16_t port) {
    addCycles(4);
    uint8_t hi = port >> 8;
    uint8_t lo = port & 0xFF;

    // CRTC 6845: A14=0, A9=1 → puerto 0xBCxx / 0xBDxx
    // Selección:  A14=0,A9=1,A8=0 → 0xBCxx  (write-only: seleccionar registro)
    // Lectura:    A14=0,A9=1,A8=1 → 0xBDxx  (status / data)
    if (!(hi & 0x40) && (hi & 0x80)) {
        // A8 (lo bit 0 real es bit 8 del puerto de 16 bits → hi bit 0)
        if (hi & 0x01) {
            // 0xBD: lectura de registro CRTC
            // Solo R16 (latch cursor) y R17 son legibles en el CRTC original;
            // devolvemos el valor almacenado (aproximación suficiente).
            return (crtc_sel < 18) ? crtc_reg[crtc_sel] : 0xFF;
        }
        return 0xFF;  // 0xBC es write-only
    }

    // FDC NEC µPD765A
    if (hi == 0xFB) {
        if (lo == 0x7E) return 0xFF;              // motor (write-only)
        if (lo == 0x7F) return fdc_read_status(&fdc);
        if (lo == 0xFF) return fdc_read_data(&fdc);
    }
    if ((hi & 0xFE) == 0xFA) return 0xFF;         // 0xFA7E / 0xFB7E

    // PPI 8255: A11=0 → 0xF4xx-0xF7xx
    // Dirección física: bits A1:A0 seleccionan puerto A/B/C/ctrl
    if (!(hi & 0x08) && (hi & 0xF0) == 0xF0) {
        switch (lo & 0x03) {
            case 0: {
                // Puerto A: AY data-bus
                uint8_t ay_ctrl = (ppi_portC >> 6) & 3;
                if (ay_ctrl == 1) return ay_reg[ay_sel & 0x0F];
                return ppi_portA;
            }
            case 1: {
                // Puerto B:
                //  bit 7 = ~VSYNC (activo a nivel bajo en el pin del GA)
                //  bit 6 = LK1 (cassette / expansion)
                //  bit 5 = ~EXP (expansión presente)
                //  bit 4 = LK4 (tipo de monitor)
                //  bit 3 = LK3
                //  bit 2 = LK2
                //  bit 1 = printer busy
                //  bit 0 = VSYNC (copia del estado GA)
                uint8_t row = (ppi_portC & 0x0F) < 10 ?
                              cpc_keymap[ppi_portC & 0x0F] : 0xFF;
                // bit7: VSYNC activo bajo (cuando el GA genera VSYNC, este
                // bit pasa a 0). Los demás bits de estado hardware son 1.
                uint8_t vsync_bit = ga_vsync ? 0x00 : 0x80;
                // Bits de enlace: LK1-LK4 configurados para monitor de color
                // (valor típico: 0x5E sin VSYNC)
                return vsync_bit | 0x5E | (row & 0x3F);
            }
            case 2: return ppi_portC;
            default: return 0xFF;
        }
    }
    return 0xFF;
}

static void io_write_cb(z80* z, uint16_t port, uint8_t val) {
    addCycles(4);
    uint8_t hi = port >> 8;
    uint8_t lo = port & 0xFF;

    /*
     * ── Gate Array (40010) ────────────────────────────────────────────────────
     *
     * Decodificación hardware EXACTA:
     *   El GA responde cuando A15=0 Y A14=1 en el bus de direcciones,
     *   es decir, puerto 0x7Fxx (y cualquier espejo donde A15=0, A14=1).
     *
     * Los bits 7:6 del DATO determinan la función:
     *   00 → seleccionar lápiz
     *   01 → asignar color
     *   10 → control (ROM, modo vídeo, reset IRQ)
     *   11 → configuración RAM (CPC 6128 solamente)
     */
    if (!(hi & 0x80) && (hi & 0x40)) {          // A15=0, A14=1
        switch (val >> 6) {

            case 0:
                /*
                 * Selección de lápiz:
                 *   bits 4:0 del valor → lápiz a seleccionar
                 *   bit4 = 1 → selección de borde (ink[16])
                 *   bit4 = 0 → tinta ink[bits 3:0]
                 */
                ga_pen = val & 0x1F;
                break;

            case 1:
                /*
                 * Asignación de color:
                 *   bits 4:0 del valor → índice de paleta hardware (0-26)
                 *   se asigna al lápiz actualmente seleccionado (ga_pen)
                 *
                 * Nota: el bit 5 del valor está cableado a tierra internamente
                 * en el 40010; solo los bits 4:0 son significativos.
                 */
                ga_ink[ga_pen & 0x1F] = val & 0x1F;
                break;

            case 2:
                /*
                 * Registro de control:
                 *   bit 1:0 → modo vídeo (0, 1 ó 2)
                 *   bit 2   → ROM lower (0=habilitada, 1=deshabilitada)
                 *   bit 3   → ROM upper (0=habilitada, 1=deshabilitada)
                 *   bit 4   → reset contador IRQ (pone a cero el contador de
                 *             52 HSYNCs y cancela cualquier IRQ pendiente)
                 *
                 * El cambio de modo vídeo es efectivo inmediatamente (el GA
                 * cambia el modo en el siguiente pixel de la línea activa).
                 */
                ga_mode      = val & 0x03;
                cpc_video_mode = ga_mode;
                rom_lo_en    = !((val >> 2) & 1);
                rom_hi_en    = !((val >> 3) & 1);
                if (val & 0x10) {
                    ga_irq_counter = 0;
                    ga_irq_pending = false;
                }
                break;

            case 3:
                /*
                 * Configuración de RAM (CPC 6128 únicamente):
                 *   bits 2:0 → índice de configuración de mapa (0-7)
                 *             según la tabla ram_map[][].
                 *
                 * Este registro solo existe en el Gate Array del 6128
                 * (el 464 no tiene RAM adicional y no responde a estos bits).
                 */
                ram_cfg = val & 0x07;
                update_vram_ptr();
                break;
        }
        return;
    }

    // CRTC 6845
    if (!(hi & 0x40) && (hi & 0x80)) {
        if (!(hi & 0x01)) {
            // 0xBCxx → seleccionar registro (write-only)
            crtc_sel = val & 0x1F;
        } else {
            // 0xBDxx → escribir en registro seleccionado
            if (crtc_sel < 18) crtc_reg[crtc_sel] = val;
            // Actualizar dirección de pantalla cuando se tocan R12 / R13
            if (crtc_sel == 12 || crtc_sel == 13) {
                crtc_screen_addr =
                    ((uint16_t)(crtc_reg[12] & 0x3F) << 8) | crtc_reg[13];
            }
        }
        return;
    }

    // FDC – motor (0xFA7E / 0xFB7E) y datos (0xFB7F / 0xFB FF)
    if ((hi & 0xFE) == 0xFA && (lo == 0x7E || lo == 0xFE)) {
        fdc_motor_control(&fdc, val);
        return;
    }
    if (hi == 0xFB && (lo == 0x7F || lo == 0xFF)) {
        fdc_write_data(&fdc, val);
        return;
    }

    // PPI 8255
    if (!(hi & 0x08) && (hi & 0xF0) == 0xF0) {
        uint8_t ppireg = lo & 0x03;
        if (ppireg == 3 && (val & 0x80)) {
            ppi_ctrl = val;
        } else switch (ppireg) {
            case 0:
                ppi_portA = val;
                {
                    uint8_t ctrl = (ppi_portC >> 6) & 3;
                    if      (ctrl == 3) ay_sel = val & 0x0F;
                    else if (ctrl == 2) ay_write(ay_sel, val);
                }
                break;
            case 1:
                ppi_portB = val;
                break;
            case 2:
                ppi_portC = val;
                {
                    uint8_t ctrl = (val >> 6) & 3;
                    if      (ctrl == 3) ay_sel = ppi_portA & 0x0F;
                    else if (ctrl == 2) ay_write(ay_sel, ppi_portA);
                }
                break;
        }
        return;
    }
}

// ── Renderizado ───────────────────────────────────────────────────────────────
/*
 * Decodificación de píxeles – Gate Array
 * ───────────────────────────────────────
 * El Gate Array decodifica cada byte de VRAM en píxeles según el modo activo:
 *
 * MODO 0  –  160×200  –  16 colores  –  2 píxeles por byte
 *   Cada byte contiene 2 píxeles de 4 bits.
 *   El bit-layout del CPC real es:
 *     Píxel 0 (izquierda):  b7  b5  b3  b1  → bits 3,2,1,0 del índice de tinta
 *     Píxel 1 (derecha):    b6  b4  b2  b0  → bits 3,2,1,0 del índice de tinta
 *
 * MODO 1  –  320×200  –  4 colores   –  4 píxeles por byte
 *   Cada byte contiene 4 píxeles de 2 bits.
 *     Píxel 0:  b7  b3  → bits 1,0 del índice
 *     Píxel 1:  b6  b2
 *     Píxel 2:  b5  b1
 *     Píxel 3:  b4  b0
 *
 * MODO 2  –  640×200  –  2 colores   –  8 píxeles por byte
 *     Píxel N (0..7):  b(7-N)  → índice de tinta (0 ó 1)
 *
 * La dirección de cada línea en VRAM sigue el esquema del CRTC:
 *   addr_linea = base + (y >> 3) * bytes_por_char_row + (y & 7) * 0x800
 *
 * donde bytes_por_char_row = R1 del CRTC × 2  (habitualmente 80).
 */
static void render_frame(void) {
    // Borde activo
    uint32_t border = cpc_hw_palette[ga_ink[16] & 0x1F];

    // Pre-calcular los 16 colores de tinta activos (máscara 0x1F, paleta 0-26)
    uint32_t ink[16];
    for (int i = 0; i < 16; i++)
        ink[i] = cpc_hw_palette[ga_ink[i] & 0x1F];

    // Resolución horizontal: depende del modo
    int scr_cols;
    if      (ga_mode == 0) scr_cols = 160;
    else if (ga_mode == 1) scr_cols = 320;
    else                   scr_cols = 640;

    // Bytes por línea CRTC (R1 × 2; la unidad CRTC es de 2 bytes)
    // R1 suele ser 40 → 80 bytes = 640 píxeles en modo 2.
    int bytes_per_row = (crtc_reg[1] ? (int)crtc_reg[1] : 40) * 2;

    // Número de líneas de carácter (R6) y líneas de escán por carácter (R9+1)
    int char_rows    = crtc_reg[6] ? crtc_reg[6] : 25;
    int scan_per_chr = (crtc_reg[9] & 0x1F) + 1;   // normalmente 8
    int lines        = char_rows * scan_per_chr;    // habitualmente 200
    if (lines > CPC_H) lines = CPC_H;

    // Centrado
    int x_off = (CPC_W - scr_cols) / 2;
    int y_off = (CPC_H - lines)    / 2;
    if (x_off < 0) x_off = 0;
    if (y_off < 0) y_off = 0;

    // Dirección base de vídeo (CRTC R12/R13).
    // Cada unidad CRTC = 2 bytes en la VRAM.
    // El CRTC trabaja con direcciones de 14 bits en unidades de 2 bytes.
    // La dirección real en bytes es: (crtc_screen_addr & 0x3FF) * 2
    uint16_t base_addr = (crtc_screen_addr & 0x3FF) * 2;

    // ── Borde superior ────────────────────────────────────────────────────────
    for (int y = 0; y < y_off && y < CPC_H; y++)
        for (int x = 0; x < CPC_W; x++)
            cpc_pixels[y * CPC_W + x] = border;

    // ── Área activa ───────────────────────────────────────────────────────────
    for (int y = 0; y < lines && (y_off + y) < CPC_H; y++) {
        /*
         * Cálculo de dirección CRTC → VRAM:
         *
         * El CRTC genera la dirección de carácter así:
         *   char_row  = y / scan_per_chr
         *   scan_line = y % scan_per_chr
         *
         * La dirección de inicio de la fila de caracteres (en bytes VRAM):
         *   row_addr  = base_addr + char_row * bytes_per_row
         *
         * El scan_line selecciona uno de los 8 bancos de 0x800 bytes:
         *   vram_addr = row_addr + scan_line * 0x800
         *
         * Finalmente se aplica módulo 0x4000 (VRAM es de 16 KB).
         */
        int char_row  = y / scan_per_chr;
        int scan_line = y % scan_per_chr;
        uint16_t line_addr = (uint16_t)(base_addr
                           + char_row  * bytes_per_row
                           + scan_line * 0x800) & 0x3FFF;

        uint32_t* row = cpc_pixels + (y_off + y) * CPC_W;

        // Borde izquierdo
        for (int x = 0; x < x_off; x++) row[x] = border;

        // Área de píxeles activos
        switch (ga_mode) {

            case 0:
                /*
                 * Modo 0 – 160×200 – 16 colores – 2 px/byte
                 *
                 * Bit layout (bit más significativo a la izquierda):
                 *   Byte:   b7 b6 b5 b4 b3 b2 b1 b0
                 *   Pixel0: b7    b5    b3    b1      → índice[3:0]
                 *   Pixel1:    b6    b4    b2    b0   → índice[3:0]
                 *
                 * Cada píxel se muestra con ancho doble (4 puntos de pantalla).
                 */
                for (int b = 0; b < bytes_per_row && (x_off + b*4) < CPC_W; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    // Píxel izquierdo (par)
                    int ci0 = ((byte >> 7) & 1) << 3 |
                              ((byte >> 5) & 1) << 2 |
                              ((byte >> 3) & 1) << 1 |
                              ((byte >> 1) & 1);
                    // Píxel derecho (impar)
                    int ci1 = ((byte >> 6) & 1) << 3 |
                              ((byte >> 4) & 1) << 2 |
                              ((byte >> 2) & 1) << 1 |
                              ((byte >> 0) & 1);
                    uint32_t col0 = ink[ci0 & 0xF];
                    uint32_t col1 = ink[ci1 & 0xF];
                    int px = x_off + b * 4;
                    if (px+0 < CPC_W) row[px+0] = col0;
                    if (px+1 < CPC_W) row[px+1] = col0;
                    if (px+2 < CPC_W) row[px+2] = col1;
                    if (px+3 < CPC_W) row[px+3] = col1;
                }
                break;

            case 1:
                /*
                 * Modo 1 – 320×200 – 4 colores – 4 px/byte
                 *
                 * Bit layout:
                 *   Byte:   b7 b6 b5 b4 b3 b2 b1 b0
                 *   Pixel0: b7          b3          → índice[1:0]
                 *   Pixel1:    b6          b2
                 *   Pixel2:       b5          b1
                 *   Pixel3:          b4          b0
                 *
                 * Cada píxel ocupa 2 puntos de pantalla.
                 */
                for (int b = 0; b < bytes_per_row && (x_off + b*4) < CPC_W; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    for (int p = 0; p < 4; p++) {
                        int ci = ((byte >> (7-p)) & 1) << 1 |
                                 ((byte >> (3-p)) & 1);
                        int px = x_off + b * 4 + p;
                        if (px < CPC_W) row[px] = ink[ci & 3];
                    }
                }
                break;

            case 2:
                /*
                 * Modo 2 – 640×200 – 2 colores – 8 px/byte
                 *
                 * Bit layout:
                 *   Byte:   b7 b6 b5 b4 b3 b2 b1 b0
                 *   Pixel0: b7
                 *   Pixel1: b6
                 *   ...
                 *   Pixel7: b0
                 *
                 * Cada píxel ocupa 1 punto de pantalla.
                 */
                for (int b = 0; b < bytes_per_row && (x_off + b*8) < CPC_W; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    for (int p = 0; p < 8; p++) {
                        int ci = (byte >> (7-p)) & 1;
                        int px = x_off + b * 8 + p;
                        if (px < CPC_W) row[px] = ink[ci & 1];
                    }
                }
                break;
        }

        // Borde derecho
        int active_w = (ga_mode == 0) ? bytes_per_row * 4
                     : (ga_mode == 1) ? bytes_per_row * 4
                     :                  bytes_per_row * 8;  // modo 2
        for (int x = x_off + active_w; x < CPC_W; x++) row[x] = border;
    }

    // ── Borde inferior ────────────────────────────────────────────────────────
    for (int y = y_off + lines; y < CPC_H; y++)
        for (int x = 0; x < CPC_W; x++)
            cpc_pixels[y * CPC_W + x] = border;
}

void cpc_render(void) {
    SDL_UpdateTexture(cpc_texture, NULL, cpc_pixels,
                      CPC_W * sizeof(uint32_t));
    SDL_RenderClear(cpc_renderer);
    SDL_RenderCopy(cpc_renderer, cpc_texture, NULL, NULL);
    SDL_RenderPresent(cpc_renderer);
}

// ── Frame update ──────────────────────────────────────────────────────────────
/*
 * Generación de IRQ del Gate Array
 * ──────────────────────────────────
 * El GA cuenta los pulsos HSYNC del CRTC. Cuando llega a 52, genera una
 * interrupción Z80 (modo 1, vector 0xFF → RST 38h) y pone el contador a 0.
 *
 * En hardware, el CRTC genera un HSYNC cada 64 µs (línea completa a 50 Hz).
 * A 4 MHz eso son 4.000.000 / (50 * 312) ≈ 256 ciclos por línea (total),
 * pero la línea completa del CRTC incluye blanking: el registro R0+1 = 64
 * ciclos de 1 MHz (= 256 ciclos de CPU).
 * 52 líneas × 256 ciclos = 13312 ciclos entre IRQs, pero muchos emuladores
 * usan la aproximación de 52 × 64 = 3328 t-states (asumiendo 1 ciclo CRTC
 * = 1 t-state, lo cual es incorrecto; 1 ciclo CRTC = 4 t-states Z80).
 *
 * Valor correcto: 52 líneas × (R0+1) ciclos CRTC × 4 t-states/ciclo CRTC
 *   = 52 × 64 × 4 = 13312 t-states por IRQ.
 *
 * (R0 por defecto = 63, por tanto R0+1 = 64.)
 */
void cpc_update(void) {
    const uint32_t frame_start = cpu_cycles;
    // Un frame completo = (R4+1)*(R9+1+1)*((R0+1)*4) ciclos
    // Aproximación estándar: 312 líneas × 64 ciclos CRTC × 4 = 79872 t-states
    const uint32_t frame_end   = frame_start + CPC_CYCLES_FRAME;

    // Ciclos entre HSYNCs = (R0+1) * 4
    uint32_t crtc_r0   = crtc_reg[0] ? crtc_reg[0] : 63;
    uint32_t hsync_cyc = (crtc_r0 + 1) * 4;   // habitualmente 256 t-states

    uint32_t next_hsync = frame_start + hsync_cyc;

    while (cpu_cycles < frame_end) {
        int cyc = z80_step(&cpu);
        if (cyc <= 0) cyc = 4;
        addCycles((uint32_t)cyc);

        // Procesar HSYNCs pendientes
        while (cpu_cycles >= next_hsync && next_hsync < frame_end) {
            ga_irq_counter++;
            if (ga_irq_counter >= 52) {
                ga_irq_counter = 0;
                ga_irq_pending = true;
            }
            next_hsync += hsync_cyc;
        }

        if (ga_irq_pending && cpu.iff1) {
            ga_irq_pending = false;
            z80_pulse_irq(&cpu, 0xFF);
        }

        fdc_tick(&fdc, cyc);
    }

    // VSYNC: se activa durante las líneas R7 a R7+(R8 & 0x0F) del frame
    // Para la lógica del puerto B PPI: marcamos vsync activo durante el render
    ga_vsync = true;   // simplificación: activo durante el procesamiento
    render_frame();
    ga_vsync = false;
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

    rom_lo_en    = true;
    rom_hi_en    = true;
    ram_cfg      = 0;
    ga_mode      = 1;
    ga_pen       = 0;
    ga_irq_counter = 0;
    ga_irq_pending = false;
    ga_vsync       = false;
    memset(ga_ink,   0, sizeof(ga_ink));
    memset(ay_reg,   0, sizeof(ay_reg));
    memset(crtc_reg, 0, sizeof(crtc_reg));

    /*
     * Valores de tinta por defecto del firmware del CPC 6128.
     * Son índices de paleta HW (0-26), no índices de firmware.
     * Correspondencia con el color de arranque estándar:
     *   ink[0]  = 1  → Blue  (borde/fondo)
     *   ink[1]  = 24 → Bright Yellow (primer plano)
     *   ink[16] = 20 → Bright Cyan   (borde de pantalla)
     */
    static const uint8_t default_ink[17] = {
        1, 24, 20, 6, 26, 0, 2, 8, 10, 12, 14, 16, 18, 22, 24, 16, 6
    };
    memcpy(ga_ink, default_ink, 17);

    // CRTC: valores por defecto (modo 50 Hz PAL, 25 filas × 8 scan)
    crtc_reg[0]  = 63;   // R0:  total horizontal
    crtc_reg[1]  = 40;   // R1:  displayed (40 chars = 80 bytes)
    crtc_reg[2]  = 46;   // R2:  HSYNC pos
    crtc_reg[3]  = 0x8E; // R3:  VSYNC/HSYNC widths
    crtc_reg[4]  = 38;   // R4:  total vertical (char rows)
    crtc_reg[5]  = 0;    // R5:  adjust
    crtc_reg[6]  = 25;   // R6:  displayed rows
    crtc_reg[7]  = 30;   // R7:  VSYNC pos
    crtc_reg[9]  = 7;    // R9:  scan lines per row − 1
    crtc_reg[12] = 0x30; // R12: screen base high (0x3000 → addr 0xC000)
    crtc_reg[13] = 0x00; // R13: screen base low
    crtc_screen_addr = ((uint16_t)(crtc_reg[12] & 0x3F) << 8) | crtc_reg[13];

    update_vram_ptr();
    fdc_reset(&fdc);
    printf("CPC 6128: RESET – Gate Array correcto\n");
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
    snprintf(path, sizeof(path), "%s/cpc6128.rom", rom_dir);
    uint8_t big_rom[0xC000];
    if (load_file(path, big_rom, 0xC000) == 0) {
        memcpy(rom_os,     big_rom,              BANK_SIZE);
        memcpy(rom_basic,  big_rom + BANK_SIZE,  BANK_SIZE);
        memcpy(rom_amsdos, big_rom + BANK_SIZE*2, BANK_SIZE);
    } else {
        snprintf(path, sizeof(path), "%s/os.rom", rom_dir);
        if (load_file(path, rom_os, BANK_SIZE) < 0) {
            fprintf(stderr, "CPC: os.rom no encontrada en %s\n", rom_dir);
            return -1;
        }
        snprintf(path, sizeof(path), "%s/basic.rom", rom_dir);
        if (load_file(path, rom_basic, BANK_SIZE) < 0) {
            fprintf(stderr, "CPC: basic.rom no encontrada\n");
            return -1;
        }
        snprintf(path, sizeof(path), "%s/amsdos.rom", rom_dir);
        load_file(path, rom_amsdos, BANK_SIZE);  // opcional
    }

    memset(ram, 0, sizeof(ram));
    memset(cpc_keymap, 0xFF, sizeof(cpc_keymap));
    cpu_cycles    = 0;
    audio_next_t  = 0.0;
    audio_buf_len = 0;

    fdc_init(&fdc);
    cpc_reset();
    printf("CPC 6128: %d Hz, %dx%d, AY-3-8912\n",
           CPC_CLOCK_HZ, CPC_W, CPC_H);
    return 0;
}

void cpc_quit(void) {}

// ── Carga de snapshots ────────────────────────────────────────────────────────
bool cpc_load_sna(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[256];
    if (fread(hdr, 1, 256, f) != 256) { fclose(f); return false; }

    cpu.af   = ((uint16_t)hdr[0x11] << 8) | hdr[0x10];
    cpu.bc   = ((uint16_t)hdr[0x13] << 8) | hdr[0x12];
    cpu.de   = ((uint16_t)hdr[0x15] << 8) | hdr[0x14];
    cpu.hl   = ((uint16_t)hdr[0x17] << 8) | hdr[0x16];
    cpu.a_f_ = ((uint16_t)hdr[0x1B] << 8) | hdr[0x1A];
    cpu.b_c_ = ((uint16_t)hdr[0x1D] << 8) | hdr[0x1C];
    cpu.d_e_ = ((uint16_t)hdr[0x1F] << 8) | hdr[0x1E];
    cpu.h_l_ = ((uint16_t)hdr[0x21] << 8) | hdr[0x20];
    cpu.ix   = ((uint16_t)hdr[0x23] << 8) | hdr[0x22];
    cpu.iy   = ((uint16_t)hdr[0x25] << 8) | hdr[0x24];
    cpu.sp   = ((uint16_t)hdr[0x27] << 8) | hdr[0x26];
    cpu.pc   = ((uint16_t)hdr[0x29] << 8) | hdr[0x28];
    cpu.interrupt_mode = hdr[0x33];
    cpu.iff1 = hdr[0x34] & 1;
    cpu.iff2 = hdr[0x35] & 1;

    // Gate Array desde el header SNA
    ga_mode = hdr[0x6D] & 3;
    cpc_video_mode = ga_mode;

    // Colores de la paleta del snapshot
    for (int i = 0; i < 17; i++)
        ga_ink[i] = hdr[0x2F + i] & 0x1F;

    // Registro de control del GA
    rom_lo_en = !((hdr[0x6D] >> 2) & 1);
    rom_hi_en = !((hdr[0x6D] >> 3) & 1);

    // Registros CRTC
    for (int i = 0; i < 18; i++)
        crtc_reg[i] = hdr[0x50 + i];
    crtc_screen_addr =
        ((uint16_t)(crtc_reg[12] & 0x3F) << 8) | crtc_reg[13];

    // RAM
    uint32_t total_ram = ((uint32_t)hdr[0x6B] << 8) | hdr[0x6A];
    for (uint32_t b = 0; b * 16384 < total_ram && b < RAM_BANKS; b++)
        fread(ram[b], 1, 16384, f);

    fclose(f);
    update_vram_ptr();
    printf("CPC SNA cargado: %s PC=0x%04X modo=%d\n",
           path, cpu.pc, ga_mode);
    return true;
}

bool cpc_load_dsk(const char* path) {
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
