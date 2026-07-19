#include "kungfum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Configuración Z80                                                         */
/* ------------------------------------------------------------------------- */
//#ifdef KFM_USE_JGZ80_API
#define KFM_Z80_INIT(cpu)         z80_init(&(cpu))
#define KFM_Z80_RESET(cpu)        z80_reset(&(cpu))
#define KFM_Z80_EXEC(cpu, cyc)    z80_step_n(&(cpu), (unsigned)(cyc))
#define KFM_Z80_IRQ_PULSE(cpu)    z80_pulse_irq(&(cpu), 0xFF)
#define KFM_Z80_IRQ_ASSERT(cpu)   z80_assert_irq(&(cpu), 0xFF)
#define KFM_Z80_IRQ_CLEAR(cpu)    z80_clr_irq(&(cpu))
/*#else
#define KFM_Z80_INIT(cpu)         ((void)(cpu))
#define KFM_Z80_RESET(cpu)        ((void)(cpu))
#define KFM_Z80_EXEC(cpu, cyc)    ((void)(cpu),(void)(cyc))
#define KFM_Z80_IRQ_PULSE(cpu)    ((void)(cpu))
#define KFM_Z80_IRQ_ASSERT(cpu)   ((void)(cpu))
#define KFM_Z80_IRQ_CLEAR(cpu)    ((void)(cpu))
#endif
*/

/* ------------------------------------------------------------------------- */
/* Depuración                                                                */
/* ------------------------------------------------------------------------- */
#define KFM_TRACE_WRITES              0
#define KFM_TRACE_LIMIT               256
#define KFM_TRACE_WARN_NO_ATTRS_FR    60
#define KFM_TRACE_WARN_STUCK_PC_FR    60

/* 0 = pulse_irq(), 1 = assert_irq()/clr_irq() */
#define KFM_IRQ_MODE_ASSERT           1

#if KFM_TRACE_WRITES
static unsigned g_trace_frame = 0;
static unsigned g_trace_tileram_writes = 0;
static unsigned g_trace_tileram_reads = 0;
static unsigned g_trace_tileram_nz_writes = 0;
static unsigned g_trace_tileram_code_writes = 0;
static unsigned g_trace_tileram_attr_writes = 0;
static unsigned g_trace_scroll_writes = 0;
static unsigned g_trace_port_writes = 0;
static unsigned g_trace_port_reads = 0;
static unsigned g_trace_textram_writes = 0; /* Kung-Fu no usa textram */
static unsigned g_trace_workram_writes = 0;
static unsigned g_trace_detail_count = 0;
static unsigned g_trace_noattr_frames = 0;
static unsigned g_trace_samepc_frames = 0;
static uint16_t g_trace_last_pc = 0xFFFF;
static int      g_trace_enabled = 1;
#endif

/* ------------------------------------------------------------------------- */
/* Raster lógico M62 / ventana visible de Kung-Fu                            */
/* ------------------------------------------------------------------------- */
#define KFM_RASTER_W         512
#define KFM_RASTER_H         256

#define KFM_VISIBLE_X0       128
#define KFM_VISIBLE_X1       383
#define KFM_VISIBLE_Y0       0
#define KFM_VISIBLE_Y1       255

static inline int kfm_visible_x(int world_x)
{
    return world_x - KFM_VISIBLE_X0;
}

static inline int kfm_visible_y(int world_y)
{
    return world_y - KFM_VISIBLE_Y0;
}

/* ------------------------------------------------------------------------- */
/* Prototipos                                                                */
/* ------------------------------------------------------------------------- */
static uint8_t mem_read(void *ud, uint16_t addr);
static void    mem_write(void *ud, uint16_t addr, uint8_t val);
static uint8_t port_in(z80 *cpu, uint16_t port);
static void    port_out(z80 *cpu, uint16_t port, uint8_t val);

static int  load_file(uint8_t *dst, int max_size, const char *path, int offset, int size);
static void audio_cb(void *userdata, Uint8 *stream, int len);
static void rebuild_palette(Kungfum *k);
static void clear_fb(Kungfum *k, uint32_t color);

static void decode_bg(Kungfum *k);
static void decode_sprites(Kungfum *k);

static void draw_bg_tile(Kungfum *k, int code, int color, int flipx, int sx, int sy);
static void draw_sprite_tile(Kungfum *k, int code, int color, int flipx, int flipy, int sx, int sy);
static void render_bg_pass(Kungfum *k, bool high_priority);
static void render_sprites(Kungfum *k);
static int  spr_height_from_prom(Kungfum *k, int code);

static void print_usage(const char *exe);
static inline void set_low_active(uint8_t *reg, int bit, bool pressed);

/* BG mapping helpers */
static int bg_addr_to_off(uint16_t addr);
static inline int bg_is_code_off(int off);
static inline int bg_is_attr_off(int off);

#if KFM_TRACE_WRITES
static void trace_tileram_write(uint16_t addr, uint16_t off, uint8_t val);
static void trace_tileram_nz_write(uint16_t addr, uint16_t off, uint8_t val);
static void trace_tileram_read(uint16_t addr, uint16_t off, uint8_t val);
static void trace_port_write(uint8_t port, uint8_t val);
static void trace_port_read(uint8_t port, uint8_t val);
static void trace_scroll_write(const char *name, uint8_t tag, uint8_t val);
static void trace_workram_write(uint16_t addr, uint16_t off, uint8_t val);
static void trace_irq(Kungfum *k, const char *kind);
static void trace_frame_summary(Kungfum *k);
#endif

/* ------------------------------------------------------------------------- */
/* BG RAM mapping - Kung Fu Master                                           */
/* ------------------------------------------------------------------------- */
/*
 * Kung-Fu:
 *   A000 = scroll low
 *   B000 = scroll high
 *   C000-C0FF = spriteram
 *   D000-D7FF = bg codes
 *   D800-DFFF = bg attrs
 *   E000-EFFF = workram
 *
 * Internamente:
 *   0x0000-0x07ff -> codes
 *   0x0800-0x0fff -> attrs
 */
static int bg_addr_to_off(uint16_t addr)
{
    if (addr >= 0xD000 && addr <= 0xD7FF)
        return (int)(addr - 0xD000);

    if (addr >= 0xD800 && addr <= 0xDFFF)
        return 0x0800 + (int)(addr - 0xD800);

    return -1;
}

static inline int bg_is_code_off(int off)
{
    return (off >= 0x0000 && off < 0x0800);
}

static inline int bg_is_attr_off(int off)
{
    return (off >= 0x0800 && off < 0x1000);
}

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                */
/* ------------------------------------------------------------------------- */
static int load_file(uint8_t *dst, int max_size, const char *path, int offset, int size)
{
    FILE *f;
    int n;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path);
        return -1;
    }

    if (offset < 0 || offset >= max_size) {
        fprintf(stderr, "[ROM] Offset fuera de rango: %s off=%d\n", path, offset);
        fclose(f);
        return -1;
    }

    if (size <= 0 || (offset + size) > max_size) {
        fprintf(stderr, "[ROM] Tamaño fuera de rango: %s off=%d size=%d max=%d\n",
                path, offset, size, max_size);
        fclose(f);
        return -1;
    }

    n = (int)fread(dst + offset, 1, (size_t)size, f);
    fclose(f);

    if (n != size) {
        fprintf(stderr, "[ROM] Lectura incompleta: %s esperado=%d leído=%d\n",
                path, size, n);
        return -1;
    }

    fprintf(stderr, "[ROM] OK %-28s -> off=%05X size=%04X\n", path, offset, size);
    return 0;
}

static void audio_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    memset(stream, 0, (size_t)len);
}

static inline uint8_t prom4_to_u8(uint8_t v)
{
    /*
     * MAME / Irem M62 / Kung-Fu:
     * resistencias asumidas:
     *   bit0 = 2.2k
     *   bit1 = 1k
     *   bit2 = 470
     *   bit3 = 220
     *
     * Pesos aproximados:
     *   0x0e, 0x1f, 0x43, 0x8f
     */
    int bit0 = (v >> 0) & 1;
    int bit1 = (v >> 1) & 1;
    int bit2 = (v >> 2) & 1;
    int bit3 = (v >> 3) & 1;

    int out = 0x0e * bit0 + 0x1f * bit1 + 0x43 * bit2 + 0x8f * bit3;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

static void rebuild_palette(Kungfum *k)
{
    int i;

    /*
     * Orden PROMs Kung-Fu:
     * 0x000..0x0ff  g-1j-.bin  = chars R
     * 0x100..0x1ff  b-1m-.bin  = sprites R
     * 0x200..0x2ff  g-1f-.bin  = chars G
     * 0x300..0x3ff  b-1n-.bin  = sprites G
     * 0x400..0x4ff  g-1h-.bin  = chars B
     * 0x500..0x5ff  b-1l-.bin  = sprites B
     */

    /* Paleta de background / chars */
    for (i = 0; i < 0x100; i++) {
        uint8_t r = prom4_to_u8(k->prom[0x000 + i]);
        uint8_t g = prom4_to_u8(k->prom[0x200 + i]);
        uint8_t b = prom4_to_u8(k->prom[0x400 + i]);

        k->palette[i] =
            0xFF000000u |
            ((uint32_t)r << 16) |
            ((uint32_t)g << 8)  |
            ((uint32_t)b);
    }

    /* Paleta de sprites */
    for (i = 0; i < 0x100; i++) {
        uint8_t r = prom4_to_u8(k->prom[0x100 + i]);
        uint8_t g = prom4_to_u8(k->prom[0x300 + i]);
        uint8_t b = prom4_to_u8(k->prom[0x500 + i]);

        k->palette[0x100 + i] =
            0xFF000000u |
            ((uint32_t)r << 16) |
            ((uint32_t)g << 8)  |
            ((uint32_t)b);
    }
}

static inline uint32_t bg_col(Kungfum *k, int color, int pix)
{
    /*
     * color = 0..31
     * pix   = 0..7 (3bpp)
     */
    return k->palette[((color & 0x1F) << 3) | (pix & 0x07)];
}

static inline uint32_t spr_col(Kungfum *k, int color, int pix)
{
    /*
     * Banco de sprites separado: +0x100
     */
    return k->palette[0x100 + (((color & 0x1F) << 3) | (pix & 0x07))];
}

static void clear_fb(Kungfum *k, uint32_t color)
{
    int i;
    for (i = 0; i < KFM_SCREEN_W * KFM_SCREEN_H; i++)
        k->framebuffer[i] = color;
}

/* ------------------------------------------------------------------------- */
/* Traza                                                                     */
/* ------------------------------------------------------------------------- */
#if KFM_TRACE_WRITES
static void trace_tileram_write(uint16_t addr, uint16_t off, uint8_t val)
{
    if (!g_trace_enabled) return;

    if (bg_is_code_off((int)off))      g_trace_tileram_code_writes++;
    else if (bg_is_attr_off((int)off)) g_trace_tileram_attr_writes++;

    g_trace_tileram_writes++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        const char *kind = "EXT";
        if (bg_is_code_off((int)off))      kind = "CODE";
        else if (bg_is_attr_off((int)off)) kind = "ATTR";

        fprintf(stderr,
                "[TRACE][F%06u] TILERAM  Z80=%04X  off=%04X  %-4s = %02X\n",
                g_trace_frame, addr, off, kind, val);
        g_trace_detail_count++;
    }
}

static void trace_tileram_nz_write(uint16_t addr, uint16_t off, uint8_t val)
{
    if (!g_trace_enabled || val == 0) return;

    g_trace_tileram_nz_writes++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        const char *kind = "EXT";
        if (bg_is_code_off((int)off))      kind = "CODE";
        else if (bg_is_attr_off((int)off)) kind = "ATTR";

        fprintf(stderr,
                "[TRACE][F%06u] TILENZ   Z80=%04X  off=%04X  %-4s = %02X\n",
                g_trace_frame, addr, off, kind, val);
        g_trace_detail_count++;
    }
}

static void trace_tileram_read(uint16_t addr, uint16_t off, uint8_t val)
{
    if (!g_trace_enabled) return;

    g_trace_tileram_reads++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        const char *kind = "EXT";
        if (bg_is_code_off((int)off))      kind = "CODE";
        else if (bg_is_attr_off((int)off)) kind = "ATTR";

        fprintf(stderr,
                "[TRACE][F%06u] TILERD   Z80=%04X  off=%04X  %-4s = %02X\n",
                g_trace_frame, addr, off, kind, val);
        g_trace_detail_count++;
    }
}

static void trace_port_write(uint8_t port, uint8_t val)
{
    if (!g_trace_enabled) return;

    g_trace_port_writes++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        fprintf(stderr,
                "[TRACE][F%06u] PORTOUT  port=%02X  val=%02X\n",
                g_trace_frame, port, val);
        g_trace_detail_count++;
    }
}

static void trace_port_read(uint8_t port, uint8_t val)
{
    if (!g_trace_enabled) return;

    g_trace_port_reads++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        fprintf(stderr,
                "[TRACE][F%06u] PORTIN   port=%02X  val=%02X\n",
                g_trace_frame, port, val);
        g_trace_detail_count++;
    }
}

static void trace_scroll_write(const char *name, uint8_t tag, uint8_t val)
{
    if (!g_trace_enabled) return;

    g_trace_scroll_writes++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        fprintf(stderr,
                "[TRACE][F%06u] SCROLL   tag=%02X  %-8s = %02X\n",
                g_trace_frame, tag, name, val);
        g_trace_detail_count++;
    }
}

static void trace_workram_write(uint16_t addr, uint16_t off, uint8_t val)
{
    if (!g_trace_enabled) return;

    g_trace_workram_writes++;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        fprintf(stderr,
                "[TRACE][F%06u] WORKRAM  Z80=%04X  off=%03X  val=%02X\n",
                g_trace_frame, addr, off, val);
        g_trace_detail_count++;
    }
}

static void trace_irq(Kungfum *k, const char *kind)
{
    if (!g_trace_enabled) return;

    if (g_trace_detail_count < KFM_TRACE_LIMIT) {
        fprintf(stderr,
                "[TRACE][F%06u] IRQ      %-8s PC=%04X\n",
                g_trace_frame, kind, k->cpu_main.pc);
        g_trace_detail_count++;
    }
}

static void trace_frame_summary(Kungfum *k)
{
    int nonzero_codes = 0;
    int nonzero_attrs = 0;
    int i;
    int hscroll;
    int vscroll;

    if (!g_trace_enabled) return;

    for (i = 0; i < 0x800; i++) {
        if (k->tileram[i] != 0x00)         nonzero_codes++;
        if (k->tileram[0x800 + i] != 0x00) nonzero_attrs++;
    }

    hscroll = (int)k->scroll_x_low | ((int)k->scroll_x_high << 8);
    vscroll = (int)k->scroll_y_low | ((int)k->scroll_y_high << 8);

    fprintf(stderr,
            "[TRACE][F%06u] SUMMARY  PC=%04X  tw=%u  tr=%u  tnz=%u  codew=%u  attrw=%u  "
            "por=%u  pow=%u  sw=%u  txw=%u  wrw=%u  "
            "codes!=0:%d  attrs!=0:%d  hscroll=%03X  vscroll=%03X\n",
            g_trace_frame,
            k->cpu_main.pc,
            g_trace_tileram_writes,
            g_trace_tileram_reads,
            g_trace_tileram_nz_writes,
            g_trace_tileram_code_writes,
            g_trace_tileram_attr_writes,
            g_trace_port_reads,
            g_trace_port_writes,
            g_trace_scroll_writes,
            g_trace_textram_writes,
            g_trace_workram_writes,
            nonzero_codes,
            nonzero_attrs,
            hscroll & 0x1FF,
            vscroll & 0x1FF);

    if (nonzero_attrs == 0) g_trace_noattr_frames++;
    else                    g_trace_noattr_frames = 0;

    if (g_trace_noattr_frames == KFM_TRACE_WARN_NO_ATTRS_FR) {
        fprintf(stderr,
                "[TRACE] WARNING: %u frames sin attrs de BG != 0\n",
                g_trace_noattr_frames);
    }

    if (k->cpu_main.pc == g_trace_last_pc) g_trace_samepc_frames++;
    else                                   g_trace_samepc_frames = 0;

    if (g_trace_samepc_frames == KFM_TRACE_WARN_STUCK_PC_FR) {
        fprintf(stderr,
                "[TRACE] WARNING: PC atascado en %04X durante %u frames\n",
                k->cpu_main.pc, g_trace_samepc_frames);
    }

    g_trace_last_pc = k->cpu_main.pc;

    g_trace_tileram_writes = 0;
    g_trace_tileram_reads = 0;
    g_trace_tileram_nz_writes = 0;
    g_trace_tileram_code_writes = 0;
    g_trace_tileram_attr_writes = 0;
    g_trace_scroll_writes = 0;
    g_trace_port_reads = 0;
    g_trace_port_writes = 0;
    g_trace_textram_writes = 0;
    g_trace_workram_writes = 0;
    g_trace_detail_count = 0;
    g_trace_frame++;
}
#endif

/* ------------------------------------------------------------------------- */
/* Decode GFX                                                                */
/* ------------------------------------------------------------------------- */
static void decode_bg(Kungfum *k)
{
    /*
     * MAME tilelayout_1024:
     * planes = { 2*NUM*8*8, NUM*8*8, 0 }
     * => bit0 = tercer bloque, bit1 = segundo, bit2 = primero
     */
    const int plane_size = KFM_GFX1_SIZE / 3;
    const uint8_t *p0 = &k->gfx1[plane_size * 2]; /* bit 0 */
    const uint8_t *p1 = &k->gfx1[plane_size * 1]; /* bit 1 */
    const uint8_t *p2 = &k->gfx1[plane_size * 0]; /* bit 2 */
    int t, y, x;

    if (!k->bg_pix) return;

    memset(k->bg_pix, 0, sizeof(KFMBgPix) * KFM_BG_COUNT);

    for (t = 0; t < KFM_BG_COUNT; t++) {
        int base = t * 8;

        for (y = 0; y < 8; y++) {
            uint8_t b0 = p0[base + y];
            uint8_t b1 = p1[base + y];
            uint8_t b2 = p2[base + y];

            for (x = 0; x < 8; x++) {
                int bit = 7 - x;
                uint8_t pen =
                    (((b0 >> bit) & 1) << 0) |
                    (((b1 >> bit) & 1) << 1) |
                    (((b2 >> bit) & 1) << 2);
                k->bg_pix[t][y][x] = pen;
            }
        }
    }
}

static void decode_sprites(Kungfum *k)
{
    /*
     * MAME spritelayout_1024:
     *
     * 16x16, 3bpp, 32 bytes por sprite
     * planes:
     *   { 2*NUM*32*8, NUM*32*8, 0 }
     *
     * x offsets:
     *   0..7, 16*8+0..7
     *
     * y offsets:
     *   0*8 .. 15*8
     *
     * Importante:
     *   La mitad derecha está en base+16+y, NO en base+y*2+1
     */
    const int plane_size = KFM_GFX2_SIZE / 3;
    const uint8_t *p0 = &k->gfx2[plane_size * 2]; /* bit 0 */
    const uint8_t *p1 = &k->gfx2[plane_size * 1]; /* bit 1 */
    const uint8_t *p2 = &k->gfx2[plane_size * 0]; /* bit 2 */
    int s, y, x;

    if (!k->spr_pix)
        return;

    memset(k->spr_pix, 0, sizeof(KFMSprPix) * KFM_SPR_COUNT);

    for (s = 0; s < KFM_SPR_COUNT; s++) {
        int base = s * 32;

        for (y = 0; y < 16; y++) {
            uint8_t l0 = p0[base + y];
            uint8_t l1 = p1[base + y];
            uint8_t l2 = p2[base + y];

            uint8_t r0 = p0[base + 16 + y];
            uint8_t r1 = p1[base + 16 + y];
            uint8_t r2 = p2[base + 16 + y];

            for (x = 0; x < 8; x++) {
                int bit = 7 - x;
                uint8_t pen =
                    (((l0 >> bit) & 1) << 0) |
                    (((l1 >> bit) & 1) << 1) |
                    (((l2 >> bit) & 1) << 2);
                k->spr_pix[s][y][x] = pen;
            }

            for (x = 0; x < 8; x++) {
                int bit = 7 - x;
                uint8_t pen =
                    (((r0 >> bit) & 1) << 0) |
                    (((r1 >> bit) & 1) << 1) |
                    (((r2 >> bit) & 1) << 2);
                k->spr_pix[s][y][8 + x] = pen;
            }
        }
    }
}

void kungfum_decode_gfx(Kungfum *k)
{
    decode_bg(k);
    decode_sprites(k);
}

/* ------------------------------------------------------------------------- */
/* Render                                                                    */
/* ------------------------------------------------------------------------- */
static void draw_bg_tile(Kungfum *k, int code, int color, int flipx, int sx, int sy)
{
    int x, y;

    if (!k->bg_pix)
        return;

    code &= (KFM_BG_COUNT - 1);

    /* sx/sy en raster lógico 512x256; se recorta a visible area */
    for (y = 0; y < 8; y++) {
        int wy = sy + y;
        if (wy < KFM_VISIBLE_Y0 || wy > KFM_VISIBLE_Y1)
            continue;

        for (x = 0; x < 8; x++) {
            int wx = sx + x;
            int fx, fy;

            if (wx < KFM_VISIBLE_X0 || wx > KFM_VISIBLE_X1)
                continue;

            fx = kfm_visible_x(wx);
            fy = kfm_visible_y(wy);

            if ((unsigned)fx >= KFM_SCREEN_W || (unsigned)fy >= KFM_SCREEN_H)
                continue;

            {
                int tx = flipx ? (7 - x) : x;
                uint8_t pen = k->bg_pix[code][y][tx];
                k->framebuffer[fy * KFM_SCREEN_W + fx] = bg_col(k, color, pen);
            }
        }
    }
}

static void draw_sprite_tile(Kungfum *k, int code, int color, int flipx, int flipy, int sx, int sy)
{
    int x, y;

    if (!k->spr_pix)
        return;

    code &= (KFM_SPR_COUNT - 1);

    /* sx/sy en raster lógico 512x256; se recorta a visible area */
    for (y = 0; y < 16; y++) {
        int wy = sy + y;
        if (wy < KFM_VISIBLE_Y0 || wy > KFM_VISIBLE_Y1)
            continue;

        for (x = 0; x < 16; x++) {
            int wx = sx + x;
            int fx, fy;

            if (wx < KFM_VISIBLE_X0 || wx > KFM_VISIBLE_X1)
                continue;

            fx = kfm_visible_x(wx);
            fy = kfm_visible_y(wy);

            if ((unsigned)fx >= KFM_SCREEN_W || (unsigned)fy >= KFM_SCREEN_H)
                continue;

            {
                int tx = flipx ? (15 - x) : x;
                int ty = flipy ? (15 - y) : y;
                uint8_t pen = k->spr_pix[code][ty][tx];

                if (pen == 0)
                    continue;

                k->framebuffer[fy * KFM_SCREEN_W + fx] = spr_col(k, color, pen);
            }
        }
    }
}

static void render_bg_pass(Kungfum *k, bool high_priority)
{
    int hscroll = (int)k->scroll_x_low | ((int)k->scroll_x_high << 8);
    int tile_index;

    hscroll &= 0x1FF;

    /*
     * Tilemap lógico = 64x32 = 512x256.
     * visible area = 128..383 x 0..255
     */
    for (tile_index = 0; tile_index < 0x800; tile_index++) {
        int code = k->tileram[tile_index];
        int attr = k->tileram[0x800 + tile_index];
        int tx   = tile_index & 0x3F;
        int ty   = tile_index >> 6;

        int full_code = code | ((attr & 0xC0) << 2);
        int pal       = attr & 0x1F;
        int flipx     = (attr & 0x20) ? 1 : 0;

        /*
         * Prioridad M62/Kung-Fu:
         * filas superiores y ciertos color codes sobre sprites.
         */
        int category  = ((ty < 6) || ((pal >> 1) > 0x0C)) ? 1 : 0;

        int sx = tx * 8;
        int sy = ty * 8;

        if (category != (high_priority ? 1 : 0))
            continue;

        /*
         * Las 6 primeras filas no hacen scroll horizontal.
         * El resto sí.
         */
        if (ty >= 6)
            sx = (sx - hscroll) & 0x1FF;

        /*
         * Se dibuja en raster lógico 512 y se dejan copias wrapped.
         * draw_bg_tile() recorta a la zona visible.
         */
        draw_bg_tile(k, full_code, pal, flipx, sx, sy);
        draw_bg_tile(k, full_code, pal, flipx, sx + 512, sy);
        draw_bg_tile(k, full_code, pal, flipx, sx - 512, sy);
    }
}

static int spr_height_from_prom(Kungfum *k, int code)
{
    /*
     * PROM de altura, una entrada por 32 sprites.
     * Mapeo práctico:
     *   0 -> 1 tile
     *   1 -> 2 tiles
     *   2 -> 4 tiles
     *   3 -> 1 tile (fallback)
     */
    uint8_t v = k->prom[0x600 + ((code >> 5) & 0x1F)] & 0x03;

    if (v == 1) return 1; /* 2 tiles total */
    if (v == 2) return 3; /* 4 tiles total */
    return 0;             /* 1 tile total */
}

static void render_sprites(Kungfum *k)
{
    int offs;

    for (offs = 0; offs + 7 < KFM_SPRITERAM_SIZE; offs += 8) {
        int code  = k->spriteram[offs + 4] | ((k->spriteram[offs + 5] & 0x07) << 8);
        int color = k->spriteram[offs + 0] & 0x1F;
        int sx    = k->spriteram[offs + 6] | ((k->spriteram[offs + 7] & 0x01) << 8);
        int syraw = k->spriteram[offs + 2] | ((k->spriteram[offs + 3] & 0x01) << 8);
        int flipx = (k->spriteram[offs + 5] & 0x40) ? 1 : 0;
        int flipy = (k->spriteram[offs + 5] & 0x80) ? 1 : 0;
        int i     = spr_height_from_prom(k, code);
        int total = i + 1;

        /* fórmula M62/Kung-Fu */
        int sy = 256 + 128 - 15 - syraw;

        if (i == 1) {
            code &= ~1;
            sy -= 16;
        } else if (i == 3) {
            code &= ~3;
            sy -= 48;
        }

        sx &= 0x1FF;
        sy &= 0x1FF;

        if (!flipy) {
            int n;
            for (n = 0; n < total; n++) {
                int draw_code = code + n;
                int draw_y = sy + n * 16;

                draw_sprite_tile(k, draw_code, color, flipx, flipy, sx, draw_y);
                draw_sprite_tile(k, draw_code, color, flipx, flipy, sx + 512, draw_y);
                draw_sprite_tile(k, draw_code, color, flipx, flipy, sx - 512, draw_y);
            }
        } else {
            int n;
            for (n = 0; n < total; n++) {
                int draw_code = code + (total - 1 - n);
                int draw_y = sy + n * 16;

                draw_sprite_tile(k, draw_code, color, flipx, flipy, sx, draw_y);
                draw_sprite_tile(k, draw_code, color, flipx, flipy, sx + 512, draw_y);
                draw_sprite_tile(k, draw_code, color, flipx, flipy, sx - 512, draw_y);
            }
        }
    }
}

void kungfum_render(Kungfum *k)
{
    clear_fb(k, k->palette[0]);
    render_bg_pass(k, false);
    render_sprites(k);
    render_bg_pass(k, true);

    SDL_UpdateTexture(k->texture, NULL, k->framebuffer,
                      KFM_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(k->renderer);
    SDL_RenderCopy(k->renderer, k->texture, NULL, NULL);
    SDL_RenderPresent(k->renderer);
}

/* ------------------------------------------------------------------------- */
/* Memoria principal                                                         */
/* ------------------------------------------------------------------------- */
static uint8_t mem_read(void *ud, uint16_t addr)
{
    Kungfum *k = (Kungfum *)ud;
    int off;

    if (addr < 0x8000)
        return k->mainrom[addr];

    off = bg_addr_to_off(addr);
    if (off >= 0) {
        uint8_t v = k->tileram[off];
#if KFM_TRACE_WRITES
        trace_tileram_read(addr, (uint16_t)off, v);
#endif
        return v;
    }

    if (addr >= 0xC000 && addr <= 0xC0FF)
        return k->spriteram[addr - 0xC000];

    if (addr >= 0xE000 && addr <= 0xEFFF)
        return k->workram[addr - 0xE000];

    return 0xFF;
}

static void mem_write(void *ud, uint16_t addr, uint8_t val)
{
    Kungfum *k = (Kungfum *)ud;
    int off;

    /* Scroll horizontal memory-mapped en Kung-Fu */
    if (addr == 0xA000) {
        k->scroll_x_low = val;
#if KFM_TRACE_WRITES
        trace_scroll_write("hscrollL", 0xA0, val);
#endif
        return;
    }

    if (addr == 0xB000) {
        k->scroll_x_high = val;
#if KFM_TRACE_WRITES
        trace_scroll_write("hscrollH", 0xB0, val);
#endif
        return;
    }

    off = bg_addr_to_off(addr);
    if (off >= 0) {
        k->tileram[off] = val;
#if KFM_TRACE_WRITES
        trace_tileram_write(addr, (uint16_t)off, val);
        trace_tileram_nz_write(addr, (uint16_t)off, val);
#endif
        return;
    }

    if (addr >= 0xC000 && addr <= 0xC0FF) {
        k->spriteram[addr - 0xC000] = val;
        return;
    }

    if (addr >= 0xE000 && addr <= 0xEFFF) {
        uint16_t woff = (uint16_t)(addr - 0xE000);
        k->workram[woff] = val;
#if KFM_TRACE_WRITES
        trace_workram_write(addr, woff, val);
#endif
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Puertos                                                                   */
/* ------------------------------------------------------------------------- */
static uint8_t port_in(z80 *cpu, uint16_t port)
{
    Kungfum *k = (Kungfum *)cpu->userdata;
    uint8_t p = (uint8_t)(port & 0xFF);
    uint8_t v;

    /*
     * MAME / Kung-Fu:
     *   00 = IN0 (start/service/coin1)
     *   01 = IN1 (P1)
     *   02 = IN2 (P2 / coin2)
     *   03 = DSW1
     *   04 = DSW2
     */
    switch (p) {
        case 0x00: v = k->in0;  break;
        case 0x01: v = k->in1;  break;
        case 0x02: v = k->in2;  break;
        case 0x03: v = k->dsw1; break;
        case 0x04: v = k->dsw2; break;
        default:   v = 0xFF;    break;
    }

#if KFM_TRACE_WRITES
    trace_port_read(p, v);
#endif
    return v;
}

static void port_out(z80 *cpu, uint16_t port, uint8_t val)
{
    Kungfum *k = (Kungfum *)cpu->userdata;
    uint8_t p = (uint8_t)(port & 0xFF);

#if KFM_TRACE_WRITES
    trace_port_write(p, val);
#endif

    switch (p) {
        case 0x00:
            k->sound_latch = val;
            break;

        case 0x01:
            /*
             * MAME:
             * data ^= ~readinputport(4) & 1;
             * se combina con DSW2 bit 0 (Flip Screen)
             */
            k->flip_screen = (((val ^ ((~k->dsw2) & 0x01)) & 0x01) != 0);
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* Init / Destroy                                                            */
/* ------------------------------------------------------------------------- */
void kungfum_init(Kungfum *k)
{
    memset(k, 0, sizeof(*k));

    /* inputs activos en bajo */
    k->in0  = 0xFF;
    k->in1  = 0xFF;
    k->in2  = 0xFF;
    k->dsw1 = 0xFF;
    k->dsw2 = 0xFF;

    k->bg_pix  = (KFMBgPix *)calloc(KFM_BG_COUNT, sizeof(KFMBgPix));
    k->spr_pix = (KFMSprPix *)calloc(KFM_SPR_COUNT, sizeof(KFMSprPix));

    if (!k->bg_pix || !k->spr_pix) {
        fprintf(stderr, "Error reservando memoria para GFX decodificadas\n");
        exit(1);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        exit(1);
    }

    k->window = SDL_CreateWindow(
        "kungfum",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        KFM_SCREEN_W * KFM_SCALE, KFM_SCREEN_H * KFM_SCALE,
        0
    );
    if (!k->window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        exit(1);
    }

    k->renderer = SDL_CreateRenderer(
        k->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!k->renderer) {
        fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        exit(1);
    }

    k->texture = SDL_CreateTexture(
        k->renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        KFM_SCREEN_W, KFM_SCREEN_H
    );
    if (!k->texture) {
        fprintf(stderr, "SDL_CreateTexture error: %s\n", SDL_GetError());
        exit(1);
    }

    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        want.callback = audio_cb;
        want.userdata = k;

        k->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (k->audio_dev)
            SDL_PauseAudioDevice(k->audio_dev, 0);
    }

//#ifdef KFM_USE_JGZ80_API
    KFM_Z80_INIT(k->cpu_main);

    k->cpu_main.read_byte  = mem_read;
    k->cpu_main.write_byte = mem_write;
    k->cpu_main.port_in    = port_in;
    k->cpu_main.port_out   = port_out;
    k->cpu_main.userdata   = k;

    KFM_Z80_RESET(k->cpu_main);
//#endif
}

void kungfum_destroy(Kungfum *k)
{
    if (k->audio_dev)
        SDL_CloseAudioDevice(k->audio_dev);

    free(k->bg_pix);
    free(k->spr_pix);

    if (k->texture)  SDL_DestroyTexture(k->texture);
    if (k->renderer) SDL_DestroyRenderer(k->renderer);
    if (k->window)   SDL_DestroyWindow(k->window);

    SDL_Quit();
}

/* ------------------------------------------------------------------------- */
/* Loaders                                                                   */
/* ------------------------------------------------------------------------- */
int kungfum_load_mainrom(Kungfum *k, const char *path, int offset, int size)
{
    int r = load_file(k->mainrom, KFM_MAINROM_SIZE, path, offset, size);
    if (!r) k->have_mainrom = true;
    return r;
}

int kungfum_load_soundrom(Kungfum *k, const char *path, int offset, int size)
{
    int r = load_file(k->soundrom, KFM_SOUNDROM_SIZE, path, offset, size);
    if (!r) k->have_soundrom = true;
    return r;
}

int kungfum_load_gfx1(Kungfum *k, const char *path, int offset, int size)
{
    int r = load_file(k->gfx1, KFM_GFX1_SIZE, path, offset, size);
    if (!r) k->have_gfx1 = true;
    return r;
}

int kungfum_load_gfx2(Kungfum *k, const char *path, int offset, int size)
{
    int r = load_file(k->gfx2, KFM_GFX2_SIZE, path, offset, size);
    if (!r) k->have_gfx2 = true;
    return r;
}

int kungfum_load_prom(Kungfum *k, const char *path, int offset, int size)
{
    int r = load_file(k->prom, KFM_PROM_SIZE, path, offset, size);
    if (!r) {
        k->have_prom = true;
        rebuild_palette(k);
    }
    return r;
}

/* ------------------------------------------------------------------------- */
/* Frame                                                                     */
/* ------------------------------------------------------------------------- */
void kungfum_run_frame(Kungfum *k)
{
    int slice = KFM_CYCLES_PER_FRAME / KFM_SLICES;
    int i;

#if KFM_IRQ_MODE_ASSERT
    for (i = 0; i < KFM_SLICES; i++) {
        if (i == KFM_SLICES - 1) {
#if KFM_TRACE_WRITES
            trace_irq(k, "assert");
#endif
            KFM_Z80_IRQ_ASSERT(k->cpu_main);
        }

        KFM_Z80_EXEC(k->cpu_main, slice);
    }

    KFM_Z80_IRQ_CLEAR(k->cpu_main);
#else
    for (i = 0; i < KFM_SLICES; i++) {
        KFM_Z80_EXEC(k->cpu_main, slice);
    }

#if KFM_TRACE_WRITES
    trace_irq(k, "pulse");
#endif
    KFM_Z80_IRQ_PULSE(k->cpu_main);
#endif

#if KFM_TRACE_WRITES
    trace_frame_summary(k);
#endif
}

/* ------------------------------------------------------------------------- */
/* Inputs                                                                    */
/* ------------------------------------------------------------------------- */
static inline void set_low_active(uint8_t *reg, int bit, bool pressed)
{
    if (pressed) *reg &= (uint8_t)~(1u << bit);
    else         *reg |= (uint8_t)(1u << bit);
}

void kungfum_handle_key(Kungfum *k, SDL_Scancode sc, bool pressed)
{
    switch (sc) {
        case SDL_SCANCODE_ESCAPE:
            if (pressed) k->quit = true;
            break;

        /* IN1 = player 1 controls */
        case SDL_SCANCODE_RIGHT: set_low_active(&k->in1, 0, pressed); break;
        case SDL_SCANCODE_LEFT:  set_low_active(&k->in1, 1, pressed); break;
        case SDL_SCANCODE_DOWN:  set_low_active(&k->in1, 2, pressed); break;
        case SDL_SCANCODE_UP:    set_low_active(&k->in1, 3, pressed); break;

        /* MAME:
         * bit 5 = button2
         * bit 7 = button1
         */
        case SDL_SCANCODE_X:     set_low_active(&k->in1, 5, pressed); break; /* button2 */
        case SDL_SCANCODE_Z:     set_low_active(&k->in1, 7, pressed); break; /* button1 */

        /* IN0 = start/service/coin1 */
        case SDL_SCANCODE_1:     set_low_active(&k->in0, 0, pressed); break; /* START1 */
        case SDL_SCANCODE_2:     set_low_active(&k->in0, 1, pressed); break; /* START2 */
        case SDL_SCANCODE_9:     set_low_active(&k->in0, 2, pressed); break; /* SERVICE1 */
        case SDL_SCANCODE_5:     set_low_active(&k->in0, 3, pressed); break; /* COIN1 */

        /* IN2 */
        case SDL_SCANCODE_6:     set_low_active(&k->in2, 4, pressed); break; /* COIN2 */

#if KFM_TRACE_WRITES
        case SDL_SCANCODE_F1:
            if (pressed) {
                g_trace_enabled = !g_trace_enabled;
                fprintf(stderr, "[TRACE] %s\n", g_trace_enabled ? "ON" : "OFF");
            }
            break;

        case SDL_SCANCODE_F2:
            if (pressed)
                trace_frame_summary(k);
            break;
#endif

        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */
static void print_usage(const char *exe)
{
    printf("Uso: %s\n\n", exe);
    printf("Carga fija de ROMs desde roms/kungfum/\n");
    printf("Controles:\n");
    printf("  Cursores = mover\n");
    printf("  Z/X      = botones\n");
    printf("  1/2      = start\n");
    printf("  5/6      = coin1/coin2\n");
    printf("  9        = service\n");
    printf("  F1       = activar/desactivar traza\n");
    printf("  F2       = imprimir resumen inmediato\n");
}

int main(int argc, char *argv[])
{
    Kungfum k;
    uint32_t frame_ms;

    (void)argc;

    kungfum_init(&k);

    if (kungfum_load_mainrom(&k, "roms/kungfum/a-4e-c.bin", 0x0000, 0x4000) ||
        kungfum_load_mainrom(&k, "roms/kungfum/a-4d-c.bin", 0x4000, 0x4000)) {
        print_usage(argv[0]);
        kungfum_destroy(&k);
        return 1;
    }

    kungfum_load_soundrom(&k, "roms/kungfum/a-3e-.bin", 0x0000, 0x2000);
    kungfum_load_soundrom(&k, "roms/kungfum/a-3f-.bin", 0x2000, 0x2000);
    kungfum_load_soundrom(&k, "roms/kungfum/a-3h-.bin", 0x4000, 0x2000);

    if (kungfum_load_gfx1(&k, "roms/kungfum/g-4c-a.bin", 0x0000, 0x2000) ||
        kungfum_load_gfx1(&k, "roms/kungfum/g-4d-a.bin", 0x2000, 0x2000) ||
        kungfum_load_gfx1(&k, "roms/kungfum/g-4e-a.bin", 0x4000, 0x2000)) {
        kungfum_destroy(&k);
        return 1;
    }

    kungfum_load_gfx2(&k, "roms/kungfum/b-4k-.bin", 0x00000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4f-.bin", 0x02000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4l-.bin", 0x04000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4h-.bin", 0x06000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-3n-.bin", 0x08000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4n-.bin", 0x0A000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4m-.bin", 0x0C000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-3m-.bin", 0x0E000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4c-.bin", 0x10000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4e-.bin", 0x12000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4d-.bin", 0x14000, 0x2000);
    kungfum_load_gfx2(&k, "roms/kungfum/b-4a-.bin", 0x16000, 0x2000);

    /* PROMs necesarias */
    if (kungfum_load_prom(&k, "roms/kungfum/g-1j-.bin", 0x000, 0x100) ||
        kungfum_load_prom(&k, "roms/kungfum/b-1m-.bin", 0x100, 0x100) ||
        kungfum_load_prom(&k, "roms/kungfum/g-1f-.bin", 0x200, 0x100) ||
        kungfum_load_prom(&k, "roms/kungfum/b-1n-.bin", 0x300, 0x100) ||
        kungfum_load_prom(&k, "roms/kungfum/g-1h-.bin", 0x400, 0x100) ||
        kungfum_load_prom(&k, "roms/kungfum/b-1l-.bin", 0x500, 0x100) ||
        kungfum_load_prom(&k, "roms/kungfum/b-5f-.bin", 0x600, 0x020)) {
        kungfum_destroy(&k);
        return 1;
    }

#if KFM_PROM_SIZE >= 0x0720
    /* PROM de video timing, opcional si el header lo permite */
    kungfum_load_prom(&k, "roms/kungfum/b-6f-.bin", 0x620, 0x100);
#endif

    kungfum_decode_gfx(&k);

    frame_ms = 1000 / KFM_FPS;

    while (!k.quit) {
        uint32_t t0 = SDL_GetTicks();
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                k.quit = true;
            } else if (ev.type == SDL_KEYDOWN) {
                kungfum_handle_key(&k, ev.key.keysym.scancode, true);
            } else if (ev.type == SDL_KEYUP) {
                kungfum_handle_key(&k, ev.key.keysym.scancode, false);
            }
        }

        kungfum_run_frame(&k);
        kungfum_render(&k);

        {
            uint32_t dt = SDL_GetTicks() - t0;
            if (dt < frame_ms)
                SDL_Delay(frame_ms - dt);
        }
    }

    kungfum_destroy(&k);
    return 0;
}