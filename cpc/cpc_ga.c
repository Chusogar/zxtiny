/*
 * cpc_ga.c  –  Gate Array del Amstrad CPC reescrito según el enfoque de CPCEC
 *              (https://github.com/cpcitor/cpcec)
 *
 * Diferencias clave respecto a la implementación anterior:
 *
 *  · Paleta de colores basada en la tabla video_table[32] de CPCEC con
 *    componentes G/R/B independientes y corrección gamma 1.6
 *  · Tablas de lookup precalculadas (gate_mode0, gate_mode1) para la
 *    decodificación byte→píxel, idénticas al enfoque de CPCEC
 *  · video_clut[32]: CLUT precalculada (16 tintas + borde + sprites)
 *    que se actualiza solo cuando cambia la paleta, no cada frame
 *  · Renderizado con video_target (puntero al píxel actual) que avanza
 *    horizontalmente durante la generación del frame, igual que CPCEC
 *  · IRQ: counter de 52 HSYNCs, reset-by-VSYNC con lógica <32/>=32
 *    documentada en CPCEC (gate_count_r3y / irq_delay)
 *  · Decodificación de puertos exacta: A15=0 && A14=1 → GA (0x7Fxx)
 *  · RAM banking CPC 6128: 8 configs con la tabla mmu_ram[][] de CPCEC
 *  · Registro de modo por línea (mid-line mode changes)
 */

#include "cpc.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Constantes de pantalla (según CPCEC) ─────────────────────────────────── */
/*
 * CPCEC usa una resolución interna de:
 *   VIDEO_LENGTH_X = 64<<4 = 1024 pasos horizontales
 *   VIDEO_LENGTH_Y = 39<<4 = 624  pasos verticales
 * pero la ventana visible (VIDEO_PIXELS_X/Y) es menor.
 * Adaptamos esto al framebuffer CPC_W×CPC_H del emulador original.
 */
#define GA_SCAN_LINES   312     /* líneas PAL totales por frame                */
#define GA_IRQ_PERIOD    52     /* HSYNCs entre interrupciones                 */

/* ── Paleta hardware (basada en video_table de CPCEC) ─────────────────────── */
/*
 * CPCEC almacena 32 entradas de color en formato 0xRRGGBB y aplica gamma 1.6
 * separada por canal usando la sub-tabla de 16 niveles de G/R/B:
 *
 *   gamma_lut[16] = {0x00,0x2F,0x48,0x5D,0x70,0x80,0x90,0x9E,
 *                    0xAC,0xB9,0xC6,0xD2,0xDE,0xE9,0xF4,0xFF}
 *
 * Cada color se construye combinando tres índices (0,1 o 2 → negro/medio/pleno)
 * en la sub-tabla.  Los 32 colores del Gate Array (índices 0-31 en el registro
 * del hardware, aunque solo 0-26 son válidos en el 464/6128) se mapean así.
 *
 * Para mantener compatibilidad con el framebuffer ARGB del emulador original
 * precalculamos directamente los 32 colores con gamma 1.6 aplicada.
 *
 * Nota: los índices 0 y 1 del hardware son ambos el mismo color "White" (como
 * en el chip 40010 real); CPCEC los trata igual.
 */
static const uint8_t ga_gamma_lut[16] = {
    0x00, 0x2F, 0x48, 0x5D, 0x70, 0x80, 0x90, 0x9E,
    0xAC, 0xB9, 0xC6, 0xD2, 0xDE, 0xE9, 0xF4, 0xFF
};

/*
 * Para cada uno de los 32 índices de color hardware del GA, CPCEC usa:
 *   R = ga_gamma_lut[r_idx]  donde r_idx ∈ {0, 8, 15}
 *   G = ga_gamma_lut[g_idx]  donde g_idx ∈ {0, 8, 15}
 *   B = ga_gamma_lut[b_idx]  donde b_idx ∈ {0, 8, 15}
 *
 * La tabla siguiente codifica los tres índices (R_IDX, G_IDX, B_IDX) para
 * cada uno de los 32 colores hardware (índices 0x00-0x1F del registro GA).
 * Usa el mismo mapeo que video_table[32] de CPCEC.
 *
 * Formato: {r_idx, g_idx, b_idx}  (índices en ga_gamma_lut[])
 */
static const uint8_t ga_color_components[32][3] = {
/*  0 */ { 8,  8,  8},  /* White (same as 1) */
/*  1 */ { 8,  8,  8},  /* White             */
/*  2 */ { 0, 15,  8},  /* Sea Green         */
/*  3 */ {15, 15,  8},  /* Pastel Yellow     */
/*  4 */ { 0,  0,  8},  /* Blue              */
/*  5 */ {15,  0,  8},  /* Purple            */
/*  6 */ { 0,  8,  8},  /* Cyan              */
/*  7 */ {15,  8,  8},  /* Pink              */
/*  8 */ {15,  0,  8},  /* Purple (dup)      */
/*  9 */ {15, 15,  8},  /* Pastel Yellow (d) */
/* 10 */ {15, 15,  0},  /* Bright Yellow     */
/* 11 */ {15, 15, 15},  /* Bright White      */
/* 12 */ {15,  0,  0},  /* Bright Red        */
/* 13 */ {15,  0, 15},  /* Bright Magenta    */
/* 14 */ {15,  8,  0},  /* Orange            */
/* 15 */ {15,  8, 15},  /* Pastel Magenta    */
/* 16 */ { 0,  0,  8},  /* Blue (dup)        */
/* 17 */ { 0, 15,  8},  /* Sea Green (dup)   */
/* 18 */ { 0, 15,  0},  /* Bright Green      */
/* 19 */ { 0, 15, 15},  /* Bright Cyan       */
/* 20 */ { 0,  0,  0},  /* Black             */
/* 21 */ { 0,  0, 15},  /* Bright Blue       */
/* 22 */ { 0,  8,  0},  /* Green             */
/* 23 */ { 0,  8, 15},  /* Sky Blue          */
/* 24 */ { 8,  0,  8},  /* Magenta           */
/* 25 */ { 8, 15,  8},  /* Pastel Green      */
/* 26 */ { 8, 15,  0},  /* Lime              */
/* 27 */ { 8, 15, 15},  /* Pastel Cyan       */
/* 28 */ { 8,  0,  0},  /* Red               */
/* 29 */ { 8,  0, 15},  /* Mauve             */
/* 30 */ { 8,  8,  0},  /* Yellow            */
/* 31 */ { 8,  8, 15},  /* Pastel Blue       */
};

/* CLUT precalculada (formato ARGB, igual que cpc_pixels[]) */
static uint32_t ga_video_table[32];  /* paleta base con gamma 1.6           */
static uint32_t video_clut[32];      /* CLUT activa: 16 tintas + borde [16] */

/* ── Tablas de decodificación byte→píxel (idénticas a CPCEC) ─────────────── */
/*
 * CPCEC precalcula dos tablas:
 *   gate_mode0[2][256]: para Modo 0 (4 bits/píxel → 2 píxeles por byte)
 *     gate_mode0[0][b] = índice ink del píxel par   (bits 7,5,3,1)
 *     gate_mode0[1][b] = índice ink del píxel impar (bits 6,4,2,0)
 *
 *   gate_mode1[4][256]: para Modo 1 (2 bits/píxel → 4 píxeles por byte)
 *     gate_mode1[p][b] = índice ink del píxel p (0-3)
 *
 * En Modo 2 (1 bit/píxel → 8 píxeles) no se necesita tabla: bit (7-p) directo.
 *
 * Estas tablas permiten decodificar un byte de VRAM con un único acceso
 * a memoria en lugar de operar bit a bit en el bucle de render.
 */
static uint8_t gate_mode0[2][256];  /* Modo 0: 2 píxeles de 4 bits por byte */
static uint8_t gate_mode1[4][256];  /* Modo 1: 4 píxeles de 2 bits por byte */

/* ── Estado del Gate Array ────────────────────────────────────────────────── */
static uint8_t  gate_table[17];         /* tabla de tintas: [0-15]=inks, [16]=borde */
static uint8_t  gate_index   = 0;       /* lápiz seleccionado (0-16)                */
static uint8_t  gate_mode    = 1;       /* modo vídeo actual (0/1/2)                */
static uint8_t  gate_mcr     = 0;       /* Mode/ROM/IRQ control register            */
static uint8_t  gate_ram     = 0;       /* RAM banking config (bits 2:0)            */
static uint8_t  gate_count   = 0;       /* contador de HSYNCs (0-51)                */
static bool     gate_irq     = false;   /* IRQ pendiente                            */
static bool     gate_vsync   = false;   /* VSYNC activo                             */
static uint8_t  gate_r3y     = 0;       /* contador de líneas de VSYNC              */
static uint32_t gate_scanline = 0;      /* línea de escán actual en el frame        */
static bool     ga_clut_dirty = true;   /* marcar CLUT para recálculo               */

/* tabla de modo por línea (mid-line mode changes, igual que CPCEC) */
static uint8_t  gate_mode_per_line[GA_SCAN_LINES];

/* puntero al píxel actual de salida (estilo CPCEC video_target) */
static uint32_t *video_target = NULL;

/* ── ROM/RAM config (igual que CPCEC mmu_ram[][]) ──────────────────────────── */
/*
 * CPCEC usa mmu_ram[4] y mmu_rom[4]: punteros a las páginas de 16K
 * recalculados en cada cambio de banking.  Aquí mantenemos la misma lógica
 * pero adaptada a los arrays de cpc.c.
 */
extern uint8_t ram[8][0x4000];
extern uint8_t rom_os[0x4000];
extern uint8_t rom_basic[0x4000];
extern bool    rom_lo_en;
extern bool    rom_hi_en;
extern uint8_t ram_cfg;
extern uint32_t cpc_pixels[CPC_H * CPC_W];
extern int      cpc_video_mode;

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

/* ────────────────────────────────────────────────────────────────────────────
 * ga_build_video_table()
 *
 * Construye ga_video_table[32] aplicando corrección gamma 1.6 por canal,
 * exactamente como CPCEC construye video_xlat[] a partir de video_table[].
 * ─────────────────────────────────────────────────────────────────────────── */
static void ga_build_video_table(void)
{
    for (int i = 0; i < 32; i++) {
        uint8_t r = ga_gamma_lut[ga_color_components[i][0]];
        uint8_t g = ga_gamma_lut[ga_color_components[i][1]];
        uint8_t b = ga_gamma_lut[ga_color_components[i][2]];
        ga_video_table[i] = 0xFF000000u | ((uint32_t)r << 16)
                                        | ((uint32_t)g <<  8)
                                        |  (uint32_t)b;
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_build_pixel_tables()
 *
 * Precalcula gate_mode0[2][256] y gate_mode1[4][256].
 *
 * Modo 0 – 4 bits/píxel (2 píxeles por byte):
 *   El byte de VRAM tiene el siguiente layout de bits de índice de ink:
 *     bits: 7  6  5  4  3  2  1  0
 *            B3 B2 B1 B0 -- -- -- --   (píxel par)
 *            -- -- -- -- B3 B2 B1 B0   (píxel impar)
 *   Con la codificación entrelazada del Gate Array:
 *     ci_par  = (b7<<3)|(b5<<2)|(b3<<1)|b1   = píxel izquierdo
 *     ci_impar= (b6<<3)|(b4<<2)|(b2<<1)|b0   = píxel derecho
 *
 * Modo 1 – 2 bits/píxel (4 píxeles por byte):
 *   ci_p = (b[7-p]<<1) | b[3-p]   para p=0..3
 *
 * (Modo 2 no necesita tabla: bit = (byte >> (7-p)) & 1)
 * ─────────────────────────────────────────────────────────────────────────── */
static void ga_build_pixel_tables(void)
{
    for (int b = 0; b < 256; b++) {
        /* Modo 0: píxel par e impar */
        gate_mode0[0][b] = (uint8_t)(
            (((b >> 7) & 1) << 3) |
            (((b >> 5) & 1) << 2) |
            (((b >> 3) & 1) << 1) |
            (((b >> 1) & 1)     ) );
        gate_mode0[1][b] = (uint8_t)(
            (((b >> 6) & 1) << 3) |
            (((b >> 4) & 1) << 2) |
            (((b >> 2) & 1) << 1) |
            (((b >> 0) & 1)     ) );

        /* Modo 1: 4 píxeles de 2 bits */
        for (int p = 0; p < 4; p++) {
            gate_mode1[p][b] = (uint8_t)(
                (((b >> (7 - p)) & 1) << 1) |
                (((b >> (3 - p)) & 1)     ) );
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_update_clut()
 *
 * Recalcula video_clut[] a partir de gate_table[] y ga_video_table[].
 * En CPCEC esto se llama video_xlat_clut() y se invoca desde varios puntos.
 * Solo recalculamos si ga_clut_dirty está activo (optimización de CPCEC).
 *
 * video_clut[0..15]  = tintas 0-15
 * video_clut[16]     = color de borde
 * ─────────────────────────────────────────────────────────────────────────── */
static void ga_update_clut(void)
{
    if (!ga_clut_dirty) return;
    ga_clut_dirty = false;
    for (int i = 0; i < 17; i++) {
        uint8_t hw_idx = gate_table[i] & 0x1F;
        video_clut[i] = ga_video_table[hw_idx];
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_init()
 *
 * Inicialización del Gate Array.  Debe llamarse una vez al arrancar el
 * emulador, antes de ga_reset().
 * ─────────────────────────────────────────────────────────────────────────── */
void ga_init(void)
{
    ga_build_video_table();
    ga_build_pixel_tables();
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_reset()
 *
 * Resetea el estado del Gate Array al estado inicial del hardware.
 * Valores por defecto extraídos del firmware CPC 6128.
 * ─────────────────────────────────────────────────────────────────────────── */
void ga_reset(void)
{
    gate_mode   = 1;
    gate_index  = 0;
    gate_mcr    = 0;
    gate_ram    = 0;
    gate_count  = 0;
    gate_irq    = false;
    gate_vsync  = false;
    gate_r3y    = 0;
    gate_scanline = 0;
    ga_clut_dirty = true;
    rom_lo_en   = true;
    rom_hi_en   = true;
    ram_cfg     = 0;
    cpc_video_mode = 1;

    /*
     * Tintas por defecto del firmware CPC 6128 (índices de paleta HW 0-31).
     * CPCEC inicializa gate_table[] con estos mismos valores en snap_reset().
     */
    static const uint8_t default_table[17] = {
         1, 24, 20,  6, 26,  0,  2,  8,
        10, 12, 14, 16, 18, 22, 24, 16,
         6   /* borde */
    };
    memcpy(gate_table, default_table, 17);

    memset(gate_mode_per_line, 1, sizeof(gate_mode_per_line));
    ga_update_clut();
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_write_port()
 *
 * Escribe un byte en el Gate Array.  Se llama desde io_write_cb cuando
 * A15=0 && A14=1 (puerto 0x7Fxx).
 *
 * Decodificación bits 7:6 del dato (igual que CPCEC):
 *   00 → seleccionar lápiz (gate_index)
 *   01 → asignar color (gate_table[gate_index] = val & 0x1F)
 *   10 → registro de control (MRER): modo, ROM, IRQ reset
 *   11 → configuración RAM (solo CPC 6128)
 *
 * En CPCEC el código equivalente está en el gran bloque Z80_OUT del
 * bucle principal, disperso en múltiples condiciones sobre el puerto.
 * ─────────────────────────────────────────────────────────────────────────── */
void ga_write_port(uint16_t port, uint8_t val)
{
    /* Verificar decodificación A15=0, A14=1 */
    uint8_t hi = (uint8_t)(port >> 8);
    if ((hi & 0xC0) != 0x40) return;   /* no es un acceso al GA */

    /* CPCEC también verifica A14 del puerto con (p & 0x4000); idéntico */
    if (!(port & 0x4000)) return;

    switch (val >> 6) {

        case 0:
            /*
             * Selección de lápiz:
             *   bits 4:0 → lápiz (0-15 = tinta, bit4=1 → borde [16])
             *
             * En CPCEC: gate_index = b & 31; con (b & 16) → borde
             */
            gate_index = val & 0x1F;
            break;

        case 1:
            /*
             * Asignación de color:
             *   bits 4:0 → índice de paleta hardware (0-26/31)
             *
             * En CPCEC: gate_table[gate_index] = b & 31;
             *           video_clut_value = *video_clut_index = video_clut[...];
             *
             * Aquí marcamos la CLUT como sucia para recálculo diferido,
             * que es más eficiente (CPCEC la actualiza en línea con el
             * puntero video_clut_index/video_clut_value).
             */
            gate_table[gate_index & 0x1F] = val & 0x1F;
            ga_clut_dirty = true;
            break;

        case 2:
            /*
             * Registro de control MRER (Mode/ROM/Enable Register):
             *   bits 1:0 → modo vídeo (0/1/2)
             *   bit  2   → ROM lower: 0=habilitada, 1=deshabilitada
             *   bit  3   → ROM upper: 0=habilitada, 1=deshabilitada
             *   bit  4   → reset contador IRQ
             *
             * En CPCEC: gate_mcr = b;
             *           gate_status = (gate_status & ~3) | (b & 3);  (modo)
             *           mmu_recalc();                                 (ROM)
             *           if (b & 16) { gate_count = 0; z80_irq &= ~1; } (IRQ)
             */
            gate_mcr       = val;
            gate_mode      = val & 0x03;
            cpc_video_mode = gate_mode;
            rom_lo_en      = !((val >> 2) & 1);
            rom_hi_en      = !((val >> 3) & 1);
            if (val & 0x10) {
                /* reset del contador de IRQ (bit 4 del MRER) */
                gate_count = 0;
                gate_irq   = false;
            }
            break;

        case 3:
            /*
             * Configuración de RAM (CPC 6128 únicamente):
             *   bits 2:0 → índice de la tabla ram_map[][] (0-7)
             *
             * En CPCEC: gate_ram = (b & 63) + ((~p >> 2) & ram_extra);
             *           mmu_recalc();
             *
             * La parte ram_extra es para expansiones de 1MB/2MB que aquí
             * no implementamos; usamos solo los 3 bits inferiores.
             */
            gate_ram = val & 0x07;
            ram_cfg  = gate_ram;
            break;
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_hsync()
 *
 * Procesa un pulso de HSYNC del CRTC.  Se llama una vez por línea.
 *
 * Lógica del contador de IRQ extraída de CPCEC (gate_count / gate_count_r3y):
 *   - Incrementar gate_count en cada HSYNC
 *   - Si gate_count >= 52: generar IRQ y resetear a 0
 *   - En el flanco de VSYNC (línea R7): resincronizar gate_count
 *       < 32 → gate_count = 0
 *       >= 32 → gate_count = 32  (garantiza IRQ +20 líneas después del VSYNC)
 *
 * CPCEC usa gate_count_r3y para el contador de líneas de VSYNC activo.
 * ─────────────────────────────────────────────────────────────────────────── */
bool ga_hsync(uint32_t vsync_line, uint32_t vsync_dur)
{
    /* Registrar modo activo para esta línea (mid-line mode changes) */
    if (gate_scanline < GA_SCAN_LINES)
        gate_mode_per_line[gate_scanline] = gate_mode;

    /* ── Gestión de VSYNC ─────────────────────────────────────────────────── */
    if (gate_scanline == vsync_line) {
        /*
         * Flanco de subida de VSYNC.
         *
         * CPCEC (en el bucle video):
         *   if (gate_count < 32) gate_count = 0; else gate_count = 32;
         *   z80_irq &= ~1;  // cancelar IRQ pendiente
         */
        if (gate_count < 32)
            gate_count = 0;
        else
            gate_count = 32;
        gate_irq = false;

        gate_vsync = true;
        gate_r3y   = (uint8_t)(vsync_dur ? vsync_dur : 4);
    }

    /* Desactivar VSYNC tras la duración configurada (CRTC R3[7:4]) */
    if (gate_vsync && gate_r3y > 0) {
        if (--gate_r3y == 0)
            gate_vsync = false;
    }

    /* ── Contador de IRQ ──────────────────────────────────────────────────── */
    gate_count++;
    bool irq_triggered = false;
    if (gate_count >= GA_IRQ_PERIOD) {
        gate_count = 0;
        gate_irq   = true;
        irq_triggered = true;
    }

    gate_scanline++;
    return irq_triggered;
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_frame_start()
 *
 * Prepara el estado del GA al inicio de cada frame.
 * ─────────────────────────────────────────────────────────────────────────── */
void ga_frame_start(void)
{
    gate_scanline = 0;
    gate_vsync    = false;
    gate_r3y      = 0;
    memset(gate_mode_per_line, gate_mode, sizeof(gate_mode_per_line));
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_irq_pending() / ga_irq_acknowledge()
 *
 * Consulta y consume la IRQ pendiente del Gate Array.
 * En CPCEC esto se gestiona directamente con z80_irq.
 * ─────────────────────────────────────────────────────────────────────────── */
bool ga_irq_pending(void)  { return gate_irq; }
void ga_irq_acknowledge(void) { gate_irq = false; }
bool ga_vsync_active(void) { return gate_vsync; }

/* ────────────────────────────────────────────────────────────────────────────
 * ga_render_frame()
 *
 * Renderiza un frame completo usando las tablas de lookup precalculadas,
 * siguiendo el modelo de video_target de CPCEC.
 *
 * A diferencia del modelo anterior (render_frame en cpc.c) que iteraba con
 * índices, aquí usamos un puntero de escritura avanzado linealmente, lo que
 * genera código más eficiente y facilita futuras extensiones (scanline hooks).
 *
 * Parámetros CRTC necesarios (los mismos que antes):
 *   crtc_reg[1]  = chars por línea (bytes_per_row = reg[1]*2)
 *   crtc_reg[6]  = char rows visibles
 *   crtc_reg[9]  = scanlines por char row - 1
 *   crtc_reg[12/13] = dirección base VRAM
 *
 * La VRAM es siempre el banco físico 3 (0xC000 en config estándar).
 * ─────────────────────────────────────────────────────────────────────────── */
void ga_render_frame(const uint8_t *crtc_reg,
                     const uint8_t *vram,
                     uint32_t      *pixels)
{
	printf("ga_render_frame\n");
    /* Actualizar CLUT si ha cambiado la paleta */
    ga_update_clut();

    const uint32_t border_col = video_clut[16];

    /* Parámetros CRTC */
    int bytes_per_row = (crtc_reg[1] ? (int)crtc_reg[1] : 40) * 2;
    int char_rows     = crtc_reg[6] ? (int)crtc_reg[6] : 25;
    int scan_per_chr  = (crtc_reg[9] & 0x1F) + 1;
    int lines         = char_rows * scan_per_chr;
    if (lines > CPC_H) lines = CPC_H;

    uint16_t base_addr = (uint16_t)(
        (((uint16_t)(crtc_reg[12] & 0x3F) << 8) | crtc_reg[13]) & 0x3FF
    ) * 2;

    int y_off = (CPC_H - lines) / 2;
    if (y_off < 0) y_off = 0;

    /* ── Borde superior ───────────────────────────────────────────────────── */
    video_target = pixels;
    for (int y = 0; y < y_off && y < CPC_H; y++) {
        for (int x = 0; x < CPC_W; x++)
            *video_target++ = border_col;
    }

    /* ── Área activa ──────────────────────────────────────────────────────── */
    for (int y = 0; y < lines && (y_off + y) < CPC_H; y++) {
        uint8_t line_mode = gate_mode_per_line[y < GA_SCAN_LINES ? y : 0];

        /*
         * Ancho activo según modo.
         * CPCEC define VIDEO_PIXELS_X = 48<<4 = 768 y usa un desplazamiento
         * para el borde; aquí mantenemos la misma proporción relativa al
         * framebuffer CPC_W.
         */
        int scr_cols = (line_mode == 0) ? 160
                     : (line_mode == 1) ? 320 : 640;
        int x_off    = (CPC_W - scr_cols) / 2;
        if (x_off < 0) x_off = 0;

        int char_row  = y / scan_per_chr;
        int scan_line = y % scan_per_chr;
        uint16_t line_addr = (uint16_t)(
            base_addr + char_row * bytes_per_row + scan_line * 0x800
        ) & 0x3FFF;

        /* Posicionar video_target al inicio de esta línea del framebuffer */
        video_target = pixels + (y_off + y) * CPC_W;

        /* Borde izquierdo */
        for (int x = 0; x < x_off; x++)
            *video_target++ = border_col;

        /* ── Decodificación de píxeles con tablas de lookup ─────────────── */
        switch (line_mode) {

            case 0:
                /*
                 * Modo 0: 4 bits/píxel, 2 píxeles por byte de VRAM.
                 * CPCEC (en el bucle de render de modo 0):
                 *   VIDEO_UNIT *t = video_target + x_off;
                 *   for cada byte b de VRAM:
                 *     *(t++) = video_clut[gate_mode0[0][b]];
                 *     *(t++) = video_clut[gate_mode0[0][b]];  (doble ancho)
                 *     *(t++) = video_clut[gate_mode0[1][b]];
                 *     *(t++) = video_clut[gate_mode0[1][b]];
                 */
                for (int b = 0; b < bytes_per_row; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    uint32_t col0 = video_clut[gate_mode0[0][byte] & 0xF];
                    uint32_t col1 = video_clut[gate_mode0[1][byte] & 0xF];
					printf("M0 [%d, %d] ", col0, col1);
                    /* Cada píxel ocupa 2 posiciones horizontales en modo 0 */
                    if ((x_off + b*4 + 0) < CPC_W) *video_target++ = col0;
                    if ((x_off + b*4 + 1) < CPC_W) *video_target++ = col0;
                    if ((x_off + b*4 + 2) < CPC_W) *video_target++ = col1;
                    if ((x_off + b*4 + 3) < CPC_W) *video_target++ = col1;
                }
                break;

            case 1:
                /*
                 * Modo 1: 2 bits/píxel, 4 píxeles por byte de VRAM.
                 * CPCEC:
                 *   for cada byte b:
                 *     for p in 0..3:
                 *       *(t++) = video_clut[gate_mode1[p][b]];
                 */
                for (int b = 0; b < bytes_per_row; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    for (int p = 0; p < 4; p++) {
                        uint32_t col = video_clut[gate_mode1[p][byte] & 3];
                        if ((x_off + b*4 + p) < CPC_W) *video_target++ = col;
						printf("M1 [%d] ", col);
                    }
                }
                break;

            case 2:
                /*
                 * Modo 2: 1 bit/píxel, 8 píxeles por byte.
                 * No necesita tabla de lookup; bit directo.
                 * CPCEC:
                 *   for cada byte b:
                 *     for p in 0..7:
                 *       *(t++) = video_clut[(b >> (7-p)) & 1];
                 */
                for (int b = 0; b < bytes_per_row; b++) {
                    uint8_t byte = vram[(line_addr + b) & 0x3FFF];
                    for (int p = 0; p < 8; p++) {
                        uint32_t col = video_clut[(byte >> (7 - p)) & 1];
						printf("M2 [%d] ", col);
                        if ((x_off + b*8 + p) < CPC_W) *video_target++ = col;
                    }
                }
                break;

            default:
                /* Modo inválido: dibujar borde */
                for (int x = 0; x < scr_cols && (x_off + x) < CPC_W; x++)
                    *video_target++ = border_col;
                break;
        }

        /* Borde derecho hasta el final de la línea */
        uint32_t *line_end = pixels + (y_off + y) * CPC_W + CPC_W;
        while (video_target < line_end)
            *video_target++ = border_col;
    }

    /* ── Borde inferior ───────────────────────────────────────────────────── */
    uint32_t *frame_end = pixels + CPC_H * CPC_W;
    while (video_target < frame_end)
        *video_target++ = border_col;
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_snapshot_load()
 *
 * Restaura el estado del Gate Array desde una cabecera de snapshot SNA.
 * Offsets según la especificación oficial v1/v2/v3.
 *
 * En CPCEC la carga de snapshots está en snap_load() en cpcec.c;
 * los campos del GA se leen directamente con:
 *   gate_index = header[0x2E] & 31;
 *   for (i=0;i<17;i++) gate_table[i] = header[0x2F+i] & 31;
 *   gate_mcr = header[0x40];
 *   gate_ram = header[0x41];
 *   gate_count_r3y = 26 - crtc_count_r3y; // (v3: header[0xB2])
 *   gate_count = header[0xB3] & 63;        // (v3)
 * ─────────────────────────────────────────────────────────────────────────── */
void ga_snapshot_load(const uint8_t *hdr, uint8_t version,
                      uint8_t *crtc_reg_out)
{
    gate_index = hdr[0x2E] & 0x1F;

    for (int i = 0; i < 17; i++)
        gate_table[i] = hdr[0x2F + i] & 0x1F;

    gate_mcr       = hdr[0x40];
    gate_mode      = gate_mcr & 0x03;
    cpc_video_mode = gate_mode;
    rom_lo_en      = !((gate_mcr >> 2) & 1);
    rom_hi_en      = !((gate_mcr >> 3) & 1);

    gate_ram = hdr[0x41] & 0x07;
    ram_cfg  = gate_ram;

    if (version >= 3) {
        /*
         * En CPCEC (v3):
         *   gate_count_r3y = 26 - crtc_count_r3y;  // recalculado desde 0xB2
         *   irq_delay = header[0xB2];
         *   gate_count = header[0xB3] & 63;
         */
        gate_count = hdr[0xB3] & 0x3F;
        if (gate_count > 51) gate_count = 51;
        gate_r3y   = hdr[0xB2] & 0x03;
        gate_irq   = (hdr[0xB4] & 1) != 0;
    } else {
        gate_count = 0;
        gate_r3y   = 0;
        gate_irq   = false;
    }

    ga_clut_dirty = true;
    ga_update_clut();
    memset(gate_mode_per_line, gate_mode, sizeof(gate_mode_per_line));
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_get_border_color()
 *
 * Devuelve el color de borde actual (útil para el renderizador principal).
 * ─────────────────────────────────────────────────────────────────────────── */
uint32_t ga_get_border_color(void)
{
    ga_update_clut();
    return video_clut[16];
}

/* ────────────────────────────────────────────────────────────────────────────
 * ga_get_ink_color()
 *
 * Devuelve el color de una tinta (0-15) de la CLUT actual.
 * ─────────────────────────────────────────────────────────────────────────── */
uint32_t ga_get_ink_color(int ink)
{
    ga_update_clut();
    return video_clut[ink & 0xF];
}
