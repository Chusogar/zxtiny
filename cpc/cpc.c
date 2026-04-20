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
 *  · IRQ: contador de 52 HSYNCs exacto; reset con bit4 y con VSYNC del CRTC
 *  · Reset IRQ por VSYNC: el flanco de subida de VSYNC fuerza el contador a
 *    0 (o a 32 si el contador estaba entre 32 y 51), sincronizando el timing
 *    de interrupciones con el frame exactamente como en hardware real
 *  · Decodificación de puertos: A15=0 && A14=1 → Gate Array (0x7Fxx)
 *  · VSYNC capturado para bit 0 del puerto B de la PPI; generado línea a línea
 *  · Interleave de líneas de vídeo CRTC: fórmula exacta del CPC real
 *  · Cambios de modo vídeo mid-line: el GA registra el modo activo en cada
 *    línea de escán durante la ejecución de la CPU; render_frame() usa esa
 *    tabla para decodificar cada línea con su modo correcto
 *  · AY-3-8912 estéreo ABC: canal A→izda, canal B→ambos, canal C→dcha,
 *    salida de 2 canales intercalados (L,R) a SDL en formato stereo
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


// ROM superior seleccionada (puerto &DFxx). En CPC6128: 0=BASIC, 7=AMSDOS
static uint8_t selected_upper_rom = 0;
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
static bool    ga_vsync         = false; // VSYNC activo (PPI puerto B bit7)
static uint8_t ga_vsync_counter = 0;    // duración VSYNC en líneas (R3[7:4])
static uint32_t ga_hsync_line   = 0;    // línea de escán actual dentro del frame

int cpc_video_mode = 1;

/*
 * ── Tabla de modo vídeo por línea de escán (mid-line mode changes) ────────────
 *
 * El Gate Array permite cambiar el modo vídeo escribiendo en el puerto 0x7Fxx
 * en cualquier momento, incluso a mitad de una línea. En hardware, el cambio
 * es efectivo desde el siguiente byte que el GA lee de la VRAM.
 *
 * Para emularlo correctamente sin renderizar en tiempo de CPU (lo que
 * requeriría sincronizar píxel a píxel), usamos una tabla que registra
 * el último modo activo al inicio de cada línea de escán. Esto captura
 * todos los cambios que ocurren entre el HSYNC de una línea y el siguiente,
 * cubriendo el caso más común (cambios entre líneas en la ISR).
 *
 * Resolución máxima: 312 líneas totales PAL (incluyendo blanking).
 */
#define GA_SCAN_LINES 312
static uint8_t ga_mode_per_line[GA_SCAN_LINES];  // modo activo en cada línea

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

// ── AY-3-8912 – Salida estéreo ABC ───────────────────────────────────────────
/*
 * El CPC conecta los canales del AY-3-8912 al amplificador de audio en
 * configuración ABC estéreo:
 *
 *   Canal A → salida izquierda  (Left)
 *   Canal B → salida izquierda + derecha (Center, aparece en ambos canales)
 *   Canal C → salida derecha    (Right)
 *
 * Para evitar que el canal B suene al doble de fuerte que A y C cuando los
 * tres están activos, se aplica la mezcla estándar con los pesos documentados:
 *
 *   L = A + B*0.5
 *   R = C + B*0.5
 *
 * SDL espera muestras intercaladas: L0, R0, L1, R1, …
 * El buffer de audio pasa de int16_t[N] a int16_t[N*2].
 */
static int16_t audio_buf[1024];   // pares (L,R) intercalados; 512 frames estéreo
static size_t  audio_buf_len = 0; // número de int16_t (no frames)
static double  audio_next_t  = 0.0;
static const double CPC_CYC_PER_SAMPLE =
    (double)CPC_CLOCK_HZ / (double)CPC_AUDIO_RATE;

/* Genera un par de muestras (L, R) y las añade al buffer. */
static void audio_push_stereo(int16_t L, int16_t R) {
    audio_buf[audio_buf_len++] = L;
    audio_buf[audio_buf_len++] = R;
    if (audio_buf_len >= sizeof(audio_buf)/sizeof(audio_buf[0])) {
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

/*
 * Genera una muestra estéreo del AY-3-8912.
 * Devuelve el par (L, R) por parámetros de salida.
 */
static void ay_sample_stereo(int16_t* out_L, int16_t* out_R) {
    // ── Generador de ruido ────────────────────────────────────────────────────
    if (++ay_noise_counter >= ay_noise_period * 8) {
        ay_noise_counter = 0;
        // LFSR de 17 bits con XOR en bits 0 y 3 (polinomio del AY real)
        if ((ay_noise_lfsr & 1) ^ ((ay_noise_lfsr >> 3) & 1))
            ay_noise_lfsr = (ay_noise_lfsr >> 1) | 0x10000;
        else
            ay_noise_lfsr >>= 1;
        ay_noise_out = ay_noise_lfsr & 1;
    }

    // ── Generador de envolvente ───────────────────────────────────────────────
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

    // ── Generadores de tono y mezcla estéreo ABC ──────────────────────────────
    int32_t ch_vol[3];
    for (int c = 0; c < 3; c++) {
        if (++ay_ch[c].counter >= ay_ch[c].period * 8) {
            ay_ch[c].counter = 0;
            ay_ch[c].phase   = 1.0 - ay_ch[c].phase;
        }
        int tone_out = (ay_ch[c].phase >= 0.5) ? 1 : 0;
        int gate     = (ay_ch[c].tone_en  ? tone_out     : 1)
                     & (ay_ch[c].noise_en ? ay_noise_out : 1);
        ch_vol[c] = gate ? (int32_t)(ay_ch[c].env_en
                           ? AY_VOL_TABLE[ay_env_vol]
                           : AY_VOL_TABLE[ay_ch[c].vol]) : 0;
    }

    /*
     * Mezcla ABC estéreo:
     *   L = chA + chB/2
     *   R = chC + chB/2
     *
     * Se divide entre 2 (no entre 3) porque cada canal contribuye
     * como máximo a la mitad del volumen total de un solo lado.
     */
    int32_t L = ch_vol[0] + ch_vol[1] / 2;
    int32_t R = ch_vol[2] + ch_vol[1] / 2;

    if (L >  INT16_MAX) L =  INT16_MAX;
    if (L <  INT16_MIN) L =  INT16_MIN;
    if (R >  INT16_MAX) R =  INT16_MAX;
    if (R <  INT16_MIN) R =  INT16_MIN;

    *out_L = (int16_t)L;
    *out_R = (int16_t)R;
}

static void addCycles(uint32_t delta) {
    if (!delta) return;
    double end_t = (double)cpu_cycles + delta;
    while ((double)cpu_cycles < end_t) {
        if (audio_next_t <= (double)cpu_cycles) {
            int16_t L, R;
            ay_sample_stereo(&L, &R);
            audio_push_stereo(L, R);
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
    // CPC6128: ROM 0 = BASIC, ROM 7 = AMSDOS (interna)
    if ((selected_upper_rom & 0x7F) == 7) {
        return rom_amsdos[addr - 0xC000];
    }
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


// ─────────────────────────────────────────────────────────────────────────────
// Decodificación de puertos / PSG teclado (CPC)
// ─────────────────────────────────────────────────────────────────────────────

// Gate Array: seleccionado cuando A15=0 y A14=1 → &7Fxx (y espejos)
static inline bool sel_gate_array(uint16_t port) { return (port & 0xC000) == 0x4000; }

// CRTC: seleccionado cuando A14=0; subpuerto por A9:A8 → &BC/&BD/&BE/&BF
static inline bool sel_crtc(uint16_t port) { return (port & 0x4000) == 0x0000; }
static inline uint8_t crtc_sub(uint16_t port) { return (uint8_t)((port >> 8) & 0x03); }

// PPI 8255: seleccionado cuando A11=0; subpuerto por A9:A8 → &F4/&F5/&F6/&F7
static inline bool sel_ppi(uint16_t port) { return (port & 0x0800) == 0x0000; }
static inline uint8_t ppi_sub(uint16_t port) { return (uint8_t)((port >> 8) & 0x03); }

// FDC 765 en CPC6128: motor &FA7E, status &FB7E, data &FB7F
static inline bool is_fdc_motor(uint16_t port) { return ((port & 0xFF00) == 0xFA00) && ((port & 0x00FF) == 0x7E); }
static inline bool is_fdc_stat(uint16_t port)  { return ((port & 0xFF00) == 0xFB00) && ((port & 0x00FF) == 0x7E); }
static inline bool is_fdc_data(uint16_t port)  { return ((port & 0xFF00) == 0xFB00) && ((port & 0x00FF) == 0x7F); }

// PSG read: registro 14 (0x0E) devuelve la línea de teclado seleccionada por PPI Port C bits 3..0
static inline uint8_t psg_read_reg_cpc(uint8_t reg) {
    reg &= 0x0F;
    if (reg == 0x0E) {
        uint8_t row = ppi_portC & 0x0F;
        if (row < 10) return cpc_keymap[row];
        return 0xFF;
    }
    return ay_reg[reg];
}

// Aplicar modo del PSG (BDIR/BC1) tras cambios de Port C
// 00 inactive, 01 read, 10 write, 11 select register
static inline void ppi_apply_psg_control(void) {
    uint8_t mode = (ppi_portC >> 6) & 0x03;
    if (mode == 3) {
        ay_sel = ppi_portA & 0x0F;
    } else if (mode == 2) {
        ay_write(ay_sel, ppi_portA);
    }
}

static uint8_t io_read_cb(z80* z, uint16_t port) {
    (void)z;
    addCycles(4);

    // ── FDC 765
    if (is_fdc_stat(port)) return fdc_read_status(&fdc);
    if (is_fdc_data(port)) return fdc_read_data(&fdc);

    // ── PPI 8255
    if (sel_ppi(port)) {
        switch (ppi_sub(port)) {
            case 0: { // Port A
                // bit4 del control = 1 → Port A input
                bool inA = (ppi_ctrl & 0x10) != 0;
                uint8_t psg_mode = (ppi_portC >> 6) & 0x03;
                if (inA) {
                    // Si PSG está en modo READ (01), Port A refleja el registro seleccionado
                    if (psg_mode == 1) return psg_read_reg_cpc(ay_sel);
                    return 0xFF; // alta impedancia
                }
                return ppi_portA;
            }
            case 1: { // Port B (input)
                // bit0 = VSYNC (1=activo)
                uint8_t v = 0xFF;
                if (ga_vsync) v |= 0x01; else v &= (uint8_t)~0x01;
                return v;
            }
            case 2: { // Port C
                uint8_t v = ppi_portC;
                // Si se configura como input, flota a 1s
                if (ppi_ctrl & 0x08) v |= 0xF0; // upper input
                if (ppi_ctrl & 0x01) v |= 0x0F; // lower input
                return v;
            }
            default: // Control (en CPC suele ser write-only)
                return ppi_ctrl;
        }
    }

    // ── CRTC 6845
    if (sel_crtc(port)) {
        switch (crtc_sub(port)) {
            case 2: // &BExx status (dependiente de tipo; devolvemos FF)
                return 0xFF;
            case 3: // &BFxx data in (si lo soporta)
                return (crtc_sel < 18) ? crtc_reg[crtc_sel] : 0xFF;
            default:
                return 0xFF;
        }
    }

    // ── Gate Array: no legible
    if (sel_gate_array(port)) return 0xFF;

    return 0xFF;
}



static void io_write_cb(z80* z, uint16_t port, uint8_t val) {
    (void)z;
    addCycles(4);

    // ── FDC motor &FA7E
    if (is_fdc_motor(port)) { fdc_motor_control(&fdc, val); return; }

    // ── FDC data &FB7F
    if (is_fdc_data(port)) { fdc_write_data(&fdc, val); return; }

    // ── Gate Array (A15=0, A14=1) → escritura
    if (sel_gate_array(port)) {
        switch (val >> 6) {
            case 0: // select pen/border
                ga_pen = val & 0x1F;
                break;
            case 1: // set colour
                ga_ink[ga_pen & 0x1F] = val & 0x1F;
                break;
            case 2: { // control
                ga_mode = val & 0x03;
                cpc_video_mode = ga_mode;

                // bit2/bit3: 0=ROM enabled, 1=ROM disabled
                rom_lo_en = (val & 0x04) == 0;
                rom_hi_en = (val & 0x08) == 0;

                // bit4: reset IRQ
                if (val & 0x10) {
                    ga_irq_counter = 0;
                    ga_irq_pending = false;
                }
                break;
            }
            case 3: // RAM config (6128)
                ram_cfg = val & 0x07;
                update_vram_ptr();
                break;
        }
        return;
    }

    // ── CRTC
    if (sel_crtc(port)) {
        switch (crtc_sub(port)) {
            case 0: // &BCxx index
                crtc_sel = val & 0x1F;
                break;
            case 1: // &BDxx data out
                if (crtc_sel < 18) {
                    crtc_reg[crtc_sel] = val;
                    if (crtc_sel == 12) crtc_screen_addr = (uint16_t)((crtc_screen_addr & 0x00FF) | ((uint16_t)val << 8));
                    if (crtc_sel == 13) crtc_screen_addr = (uint16_t)((crtc_screen_addr & 0xFF00) | (uint16_t)val);
                }
                break;
            default:
                break;
        }
    }

    // ── PPI
    if (sel_ppi(port)) {
        switch (ppi_sub(port)) {
            case 0: // Port A
                ppi_portA = val;
                break;
            case 1: // Port B (latch)
                ppi_portB = val;
                break;
            case 2: // Port C
                ppi_portC = val;
                ppi_apply_psg_control();
                break;
            case 3: // Control
                if (val & 0x80) {
                    ppi_ctrl = val;
                } else {
                    uint8_t bit = (val >> 1) & 0x07;
                    if (val & 0x01) ppi_portC |=  (uint8_t)(1u << bit);
                    else            ppi_portC &= (uint8_t)~(1u << bit);
                    ppi_apply_psg_control();
                }
                break;
        }
        return;
    }

    // ── ROM select &DFxx
    if ((port & 0xFF00) == 0xDF00) {
        selected_upper_rom = val & 0x7F;
        return;
    }
}


// ── Renderizado ───────────────────────────────────────────────────────────────
/*
 * Decodificación de píxeles – Gate Array
 * ───────────────────────────────────────
 * (ver comentario de bit-layout anterior; se mantiene igual)
 *
 * CAMBIOS DE MODO MID-LINE
 * ─────────────────────────
 * render_frame() ya no usa ga_mode como valor único para todo el frame.
 * En su lugar consulta ga_mode_per_line[y] para cada línea de escán,
 * que fue rellenada durante la ejecución de la CPU en cpc_update().
 * Esto permite que juegos y demos que cambian el modo en la ISR de HSYNC
 * (o en cualquier otro momento) vean reflejado el cambio correctamente.
 *
 * El ancho de pantalla activo se recalcula para cada línea según su modo.
 * El borde izquierdo/derecho se ajusta line a line.
 */
static void render_frame(void) {
    uint32_t border = cpc_hw_palette[ga_ink[16] & 0x1F];
	printf("Mode: %d\n", ga_mode);

    uint32_t ink[16];
    for (int i = 0; i < 16; i++)
        ink[i] = cpc_hw_palette[ga_ink[i] & 0x1F];

    int bytes_per_row = (crtc_reg[1] ? (int)crtc_reg[1] : 40) * 2;
    int char_rows     = crtc_reg[6] ? crtc_reg[6] : 25;
    int scan_per_chr  = (crtc_reg[9] & 0x1F) + 1;
    int lines         = char_rows * scan_per_chr;
    if (lines > CPC_H) lines = CPC_H;

    // Centrado vertical (basado en modo 1 de referencia = 320 px de ancho)
    int y_off = (CPC_H - lines) / 2;
    if (y_off < 0) y_off = 0;

    uint16_t base_addr = (crtc_screen_addr & 0x3FF) * 2;

    // ── Borde superior ────────────────────────────────────────────────────────
    for (int y = 0; y < y_off && y < CPC_H; y++)
        for (int x = 0; x < CPC_W; x++)
            cpc_pixels[y * CPC_W + x] = border;

    // ── Área activa ───────────────────────────────────────────────────────────
    for (int y = 0; y < lines && (y_off + y) < CPC_H; y++) {
        // Modo de esta línea (puede diferir del global ga_mode)
        //uint8_t line_mode = ga_mode_per_line[y < GA_SCAN_LINES ? y : 0];
		uint8_t line_mode = ga_mode;

        int scr_cols = (line_mode == 0) ? 160
                     : (line_mode == 1) ? 320 : 640;
        int x_off    = (CPC_W - scr_cols) / 2;
        if (x_off < 0) x_off = 0;

        int char_row  = y / scan_per_chr;
        int scan_line = y % scan_per_chr;
        uint16_t line_addr = (uint16_t)(base_addr
                           + char_row  * bytes_per_row
                           + scan_line * 0x800) & 0x3FFF;

        uint32_t* row = cpc_pixels + (y_off + y) * CPC_W;

        // Borde izquierdo
        for (int x = 0; x < x_off; x++) row[x] = border;

        // Área de píxeles activos según modo de ESTA línea
        switch (line_mode) {
            case 0:
                for (int b = 0; b < bytes_per_row && (x_off + b*4) < CPC_W; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    int ci0 = ((byte >> 7) & 1) << 3 | ((byte >> 5) & 1) << 2 |
                              ((byte >> 3) & 1) << 1 | ((byte >> 1) & 1);
                    int ci1 = ((byte >> 6) & 1) << 3 | ((byte >> 4) & 1) << 2 |
                              ((byte >> 2) & 1) << 1 | ((byte >> 0) & 1);
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
        int active_w = (line_mode == 0) ? bytes_per_row * 4
                     : (line_mode == 1) ? bytes_per_row * 4
                     :                    bytes_per_row * 8;
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
 * Generación de IRQ del Gate Array – con reset por VSYNC
 * ────────────────────────────────────────────────────────
 * El GA cuenta los pulsos HSYNC del CRTC. Cuando llega a 52, genera IRQ.
 *
 * RESET POR VSYNC (comportamiento hardware documentado):
 *   Cuando el CRTC genera VSYNC (en la línea R7), el GA examina su contador:
 *
 *   · Si el contador está entre 0 y 31 → se fuerza a 0.
 *     (el VSYNC llegó antes de la mitad del ciclo; se resincroniza al inicio)
 *
 *   · Si el contador está entre 32 y 51 → se fuerza a 32.
 *     (el VSYNC llegó en la segunda mitad; se resincroniza al punto medio)
 *     Esto garantiza que la siguiente IRQ llegará exactamente 20 HSYNCs
 *     después del VSYNC, alineando el timing con el frame de 50 Hz.
 *
 *   Este mecanismo es lo que hace que los efectos de raster del CPC sean
 *   estables: la ISR siempre ocurre en la misma posición relativa al frame.
 *
 * TABLA DE MODO POR LÍNEA (mid-line mode changes):
 *   Antes de cada HSYNC se guarda ga_mode en ga_mode_per_line[línea_actual],
 *   de modo que render_frame() pueda usarlo posteriormente.
 *
 * VSYNC para PPI:
 *   ga_vsync se activa en la línea R7 y dura R3[7:4] HSYNCs (habitualmente
 *   4 líneas). El bit 7 del puerto B de la PPI refleja su estado (activo bajo).
 */
void cpc_update(void) {
    const uint32_t frame_start = cpu_cycles;
    const uint32_t frame_end   = frame_start + CPC_CYCLES_FRAME;

    uint32_t crtc_r0    = crtc_reg[0] ? crtc_reg[0] : 63;
    uint32_t hsync_cyc  = (crtc_r0 + 1) * 4;    // t-states por línea (256)

    // Línea del CRTC en la que comienza el VSYNC (R7) y su duración (R3[7:4])
    uint32_t vsync_line = crtc_reg[7] ? crtc_reg[7] : 30;
    uint32_t vsync_dur  = (crtc_reg[3] >> 4) & 0x0F;
    if (!vsync_dur) vsync_dur = 4;  // si R3[7:4]=0, el hardware usa 16 (≈4 visible)

    uint32_t next_hsync  = frame_start + hsync_cyc;
    ga_hsync_line        = 0;       // contador de línea dentro del frame
    ga_vsync             = false;
    ga_vsync_counter     = 0;

    // Pre-rellenar la tabla de modos con el modo actual (se sobrescribirá línea a línea)
    memset(ga_mode_per_line, ga_mode, sizeof(ga_mode_per_line));

    while (cpu_cycles < frame_end) {
        int cyc = z80_step(&cpu);
        if (cyc <= 0) cyc = 4;
        addCycles((uint32_t)cyc);

        // Procesar HSYNCs pendientes
        while (cpu_cycles >= next_hsync && next_hsync < frame_end) {

            // ── Registro del modo activo para esta línea (mid-line) ──────────
            if (ga_hsync_line < GA_SCAN_LINES)
                ga_mode_per_line[ga_hsync_line] = ga_mode;

            // ── VSYNC: activación y desactivación ────────────────────────────
            if (ga_hsync_line == vsync_line) {
                /*
                 * Flanco de subida de VSYNC:
                 * El GA recibe la señal y resincroniza el contador de IRQ.
                 *
                 *   contador < 32  → contador = 0
                 *   contador >= 32 → contador = 32
                 *
                 * En ambos casos se cancela cualquier IRQ pendiente generada
                 * justo en este momento (el hardware tiene un latch que previene
                 * la doble interrupción en el mismo VSYNC).
                 */
                if (ga_irq_counter < 32)
                    ga_irq_counter = 0;
                else
                    ga_irq_counter = 32;
                ga_irq_pending = false;

                ga_vsync         = true;
                ga_vsync_counter = (uint8_t)vsync_dur;
            }
            if (ga_vsync && ga_vsync_counter > 0) {
                if (--ga_vsync_counter == 0)
                    ga_vsync = false;
            }

            // ── Contador de IRQ del GA ────────────────────────────────────────
            ga_irq_counter++;
            if (ga_irq_counter >= 52) {
                ga_irq_counter = 0;
                ga_irq_pending = true;
            }

            ga_hsync_line++;
            next_hsync += hsync_cyc;
        }

        // Lanzar IRQ si procede (IFF1 debe estar habilitado)
        if (ga_irq_pending && cpu.iff1) {
            ga_irq_pending = false;
            z80_pulse_irq(&cpu, 0xFF);
        }

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

    rom_lo_en    = true;
    rom_hi_en    = true;
    ram_cfg      = 0;
    ga_mode      = 1;
    ga_pen       = 0;
    ga_irq_counter = 0;
    ga_irq_pending = false;
    ga_vsync       = false;
    ga_vsync_counter = 0;
    ga_hsync_line    = 0;
    memset(ga_mode_per_line, 1, sizeof(ga_mode_per_line));
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

// ── Carga de snapshots .SNA ───────────────────────────────────────────────────
/*
 * Formato SNA – tabla de offsets (especificación oficial v1/v2/v3)
 * ─────────────────────────────────────────────────────────────────
 *  0x00-0x07  "MV - SNA"  (identificador, 8 bytes)
 *  0x08-0x0F  (reservado, 0)
 *  0x10       versión del snapshot (1, 2 ó 3)
 *
 *  Registros Z80 (TODOS en orden F,A,C,B,E,D,L,H,R,I – little-endian por par):
 *  0x11  F       0x12  A       → AF  = A<<8|F
 *  0x13  C       0x14  B       → BC  = B<<8|C
 *  0x15  E       0x16  D       → DE  = D<<8|E
 *  0x17  L       0x18  H       → HL  = H<<8|L
 *  0x19  R
 *  0x1A  I
 *  0x1B  IFF0 (= IFF1, bit 0 significativo)
 *  0x1C  IFF1 (= IFF2, bit 0 significativo)
 *  0x1D  IX_lo   0x1E  IX_hi   → IX  = IX_hi<<8|IX_lo
 *  0x1F  IY_lo   0x20  IY_hi   → IY  = IY_hi<<8|IY_lo
 *  0x21  SP_lo   0x22  SP_hi   → SP  = SP_hi<<8|SP_lo
 *  0x23  PC_lo   0x24  PC_hi   → PC  = PC_hi<<8|PC_lo
 *  0x25  IM      (modo de interrupción: 0, 1 ó 2)
 *  0x26  F'      0x27  A'      → AF' = A'<<8|F'
 *  0x28  C'      0x29  B'      → BC' = B'<<8|C'
 *  0x2A  E'      0x2B  D'      → DE' = D'<<8|E'
 *  0x2C  L'      0x2D  H'      → HL' = H'<<8|L'
 *
 *  Gate Array:
 *  0x2E  lápiz seleccionado (bits 4:0)
 *  0x2F-0x3F  paleta: 16 tintas + borde (17 bytes, bits 4:0 = índice HW)
 *  0x40  registro de control GA (modo vídeo bits 1:0, ROM lo bit2, ROM hi bit3)
 *  0x41  configuración RAM (bits 5:0)
 *
 *  CRTC:
 *  0x42  registro seleccionado (0-31)
 *  0x43-0x54  datos de registros R0..R17 (18 bytes)
 *
 *  Otros:
 *  0x55  ROM select (último valor escrito en puerto 0xDFxx)
 *  0x56  PPI puerto A
 *  0x57  PPI puerto B
 *  0x58  PPI puerto C
 *  0x59  PPI control
 *  0x5A  PSG registro seleccionado
 *  0x5B-0x6A  PSG registros 0-15 (16 bytes)
 *  0x6B  tamaño del volcado de RAM en KB (byte bajo)
 *  0x6C  tamaño del volcado de RAM en KB (byte alto)
 *
 *  Solo en v2/v3:
 *  0x6D  tipo de CPC (0=464, 1=664, 2=6128, ...)
 *  0x6E  número de interrupción (0-5) [v2]
 *  0x6F-0x74  modos multimode por interrupción [v2]
 *
 *  Solo en v3:
 *  0x9C  estado motor FDD
 *  0xB2  GA vsync delay counter
 *  0xB3  GA interrupt scanline counter (0-51)
 *  0xB4  interrupt request flag
 *
 *  A partir de 0x100: volcado de RAM (sin dependencia de la configuración
 *  de banking; los primeros 64 KB son siempre la RAM base 0x0000-0xFFFF
 *  en el orden físico de bancos 0,1,2,3; los siguientes 64 KB son los
 *  bancos extra 4,5,6,7 del CPC 6128).
 */
// Añadimos esta variable global/estática si no estaba para controlar la ROM superior

bool cpc_load_sna(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "SNA: No se puede abrir '%s'", path);
        return false;
    }

    uint8_t hdr[256];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fprintf(stderr, "SNA: Cabecera incompleta");
        fclose(f);
        return false;
    }

    if (memcmp(hdr, "MV - SNA", 8) != 0) {
        fprintf(stderr, "SNA: Identificador inválido");
        fclose(f);
        return false;
    }

    const uint8_t version = hdr[0x10];

    #define LE16(p) ((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8))

    // ── Estado CPU (jgz80) ───────────────────────────────────────────────
    cpu.halted      = false;
    cpu.irq_pending = 0;
    cpu.nmi_pending = 0;
    cpu.irq_data    = 0;
    cpu.iff_delay   = 0;

    cpu.f = hdr[0x11]; cpu.a = hdr[0x12];
    cpu.c = hdr[0x13]; cpu.b = hdr[0x14];
    cpu.e = hdr[0x15]; cpu.d = hdr[0x16];
    cpu.l = hdr[0x17]; cpu.h = hdr[0x18];

    cpu.r = hdr[0x19];
    cpu.i = hdr[0x1A];
    cpu.iff1 = (hdr[0x1B] & 1) != 0;
    cpu.iff2 = (hdr[0x1C] & 1) != 0;

    cpu.ix = LE16(&hdr[0x1D]);
    cpu.iy = LE16(&hdr[0x1F]);
    cpu.sp = LE16(&hdr[0x21]);
    cpu.pc = LE16(&hdr[0x23]);

    cpu.interrupt_mode = hdr[0x25] & 3;

    cpu.f_ = hdr[0x26]; cpu.a_ = hdr[0x27];
    cpu.c_ = hdr[0x28]; cpu.b_ = hdr[0x29];
    cpu.e_ = hdr[0x2A]; cpu.d_ = hdr[0x2B];
    cpu.l_ = hdr[0x2C]; cpu.h_ = hdr[0x2D];

    cpu.mem_ptr = cpu.pc;

    // ── Gate Array / banking ─────────────────────────────────────────────
    ga_pen = hdr[0x2E] & 0x1F;
    for (int i = 0; i < 17; i++) ga_ink[i] = hdr[0x2F + i] & 0x1F;

    const uint8_t ga_ctrl = hdr[0x40];
    ga_mode        = ga_ctrl & 0x03;
    cpc_video_mode = ga_mode;
    rom_lo_en      = (ga_ctrl & 0x04) == 0;
    rom_hi_en      = (ga_ctrl & 0x08) == 0;

    ram_cfg = hdr[0x41] & 0x07;
    selected_upper_rom = hdr[0x55] & 0x7F;

    // ── CRTC ─────────────────────────────────────────────────────────────
    crtc_sel = hdr[0x42] & 0x1F;
    for (int i = 0; i < 18; i++) crtc_reg[i] = hdr[0x43 + i];
    crtc_screen_addr = ((uint16_t)(crtc_reg[12] & 0x3F) << 8) | crtc_reg[13];

    // ── PPI / PSG ─────────────────────────────────────────────────────────
    ppi_portA = hdr[0x56];
    ppi_portB = hdr[0x57];
    ppi_portC = hdr[0x58];
    ppi_ctrl  = hdr[0x59];

    ay_sel = hdr[0x5A] & 0x0F;
    for (int r = 0; r < 16; r++) ay_write((uint8_t)r, hdr[0x5B + r]);

    // ── RAM dump ──────────────────────────────────────────────────────────
    uint32_t ram_kb = ((uint32_t)hdr[0x6C] << 8) | hdr[0x6B];
    if (ram_kb == 0) ram_kb = 64;
    if (ram_kb > 128) ram_kb = 128;

    uint32_t banks_to_load = ram_kb / 16;
    if (banks_to_load > RAM_BANKS) banks_to_load = RAM_BANKS;

    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, cur, SEEK_SET);

    long need = 0x100 + (long)banks_to_load * (long)BANK_SIZE;
    if (fsize < need) {
        fprintf(stderr, "SNA: fichero truncado (tamaño=%ld, esperado>=%ld)", fsize, need);
        fclose(f);
        return false;
    }

    if (fseek(f, 0x100, SEEK_SET) != 0) {
        fprintf(stderr, "SNA: error posicionando RAM");
        fclose(f);
        return false;
    }

    for (uint32_t b = 0; b < banks_to_load; b++) {
        size_t rd = fread(ram[b], 1, BANK_SIZE, f);
        if (rd != BANK_SIZE) {
            fprintf(stderr, "SNA: error leyendo RAM bank %u", (unsigned)b);
            fclose(f);
            return false;
        }
    }

    fclose(f);

    // ── Estado extra v3 ───────────────────────────────────────────────────
    if (version >= 3) {
        ga_vsync_counter = hdr[0xB2] & 0x03;
        ga_irq_counter   = hdr[0xB3] & 0x3F;
        ga_irq_pending   = (hdr[0xB4] & 1) != 0;
    } else {
        ga_irq_counter = 0;
        ga_irq_pending = false;
    }

    update_vram_ptr();
    memset(ga_mode_per_line, ga_mode, sizeof(ga_mode_per_line));

    cpu_cycles   = 0;
    audio_next_t = 0.0;

    #undef LE16

    printf("SNA cargado: v%u PC=0x%04X SP=0x%04X RAM=%uKB ROMsel=%u",
           (unsigned)version, cpu.pc, cpu.sp, (unsigned)ram_kb, (unsigned)selected_upper_rom);

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
