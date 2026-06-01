#include "gng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// ROM loader helpers (estilo shaolins)
// =============================================================================
static int load_rom_exact(const char *dir, const char *file,
                          uint8_t *dst, size_t off, size_t cap, size_t expect)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0 || (size_t)sz != expect || off + (size_t)sz > cap) {
        fclose(fp);
        return -2;
    }
    fread(dst + off, 1, (size_t)sz, fp);
    fclose(fp);

    printf("[ROM] %s (%ld) -> off=0x%zx\n", path, sz, off);
    return 0;
}

static int load_rom_any(const char *dir, const char **names, int n,
                        uint8_t *dst, size_t off, size_t cap, size_t expect)
{
    for (int i = 0; i < n; i++) {
        if (load_rom_exact(dir, names[i], dst, off, cap, expect) == 0)
            return 0;
    }
    fprintf(stderr, "[ROM] No encontrada ninguna variante (dir=%s) off=0x%zx size=0x%zx\n",
            dir, off, expect);
    return -1;
}

// =============================================================================
// Audio (stub): silencio
// =============================================================================
static void audio_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    memset(stream, 0, len);
}

// =============================================================================
// Paleta GnG: RRRRGGGGBBBBxxxx (split HI/LO)
//
// Hardware: palette_device con formato RGBx_444 big-endian.
// Dos bancos de RAM mapeados en el bus del MC6809:
//   0x3800-0x38FF  →  pal_ext[i]  (byte alto de la palabra de 16 bits)
//   0x3900-0x39FF  →  pal[i]      (byte bajo de la palabra de 16 bits)
//
// Palabra de color combinada: { pal_ext[i], pal[i] } = 16 bits
//   bits 15-12  →  R (4 bits)   pal_ext >> 4
//   bits 11-8   →  G (4 bits)   pal_ext & 0x0F
//   bits  7-4   →  B (4 bits)   pal   >> 4
//   bits  3-0   →  x (ignorado) pal   & 0x0F
//
// Expansión 4→8 bits: component8 = component4 * 17  (replica nibble: 0xF→0xFF)
//
// Mapa de índices de paleta (GFXDECODE de gng.cpp):
//   0x00-0x3F (0..63)   →  BG tiles  (8 grupos de color × 8 pens  = 64)
//   0x40-0x7F (64..127) →  Sprites   (4 grupos de color × 16 pens = 64)
//   0x80-0xFF (128..255)→  FG chars  (16 grupos de color × 4 pens = 64 útiles)
// =============================================================================

// Constantes de base de paleta por capa (verificadas en gng.cpp GFXDECODE)
#define GNG_PAL_BG_BASE    0x00   // tiles:   { "bgtiles", ..., &tilelayout,   0x00, 8 }
#define GNG_PAL_SPR_BASE   0x40   // sprites: { "sprites", ..., &spritelayout, 0x40, 4 }
#define GNG_PAL_FG_BASE    0x80   // chars:   { "fgtiles", ..., &charlayout,   0x80, 16}

static void palette_recalc(Gng *g)
{
    for (int i = 0; i < 256; i++) {
        uint8_t hi = g->pal_ext[i]; // 0x3800.. byte alto
        uint8_t lo = g->pal[i];     // 0x3900.. byte bajo

        // Palabra combinada {hi, lo} = RRRRGGGGBBBBxxxx
        uint8_t r4 = (hi >> 4) & 0x0F;
        uint8_t g4 = (hi >> 0) & 0x0F;
        uint8_t b4 = (lo >> 4) & 0x0F;

        // Expandir 4-bit → 8-bit replicando el nibble (0xF→0xFF, 0x0→0x00)
        uint8_t r  = (uint8_t)(r4 * 17);
        uint8_t g8 = (uint8_t)(g4 * 17);
        uint8_t b  = (uint8_t)(b4 * 17);

        g->argb[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g8 << 8) | (uint32_t)b;
    }
}



// =============================================================================
// GFX decode estilo MAME GfxLayout (como shaolins.c) [1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/gng.c)
// =============================================================================
static inline int bit_get_msb(const uint8_t *src, int bit_index)
{
    // bit 0 == MSB (0x80)
    return (src[bit_index >> 3] >> (7 - (bit_index & 7))) & 1;
}

static void decode_chars(Gng *g)
{
    static const int plane[2] = { 4, 0 };
    static const int xoff[8]  = { 0, 1, 2, 3, 8, 9, 10, 11 };
    static const int yoff[8]  = { 0, 16, 32, 48, 64, 80, 96, 112 };
    const int inc = 16 * 8;

    g->num_chars = GNG_MAX_CHARS;

    for (int c = 0; c < g->num_chars; c++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int pen = 0;
                for (int p = 0; p < 2; p++) {
                    int b = plane[p] + c*inc + yoff[y] + xoff[x];
                    pen |= bit_get_msb(g->chr, b) << p;
                }
                g->chr_pix[c][y][x] = (uint8_t)pen;
            }
        }
    }
}

static void decode_tiles(Gng *g)
{
    // 16x16, 3bpp. Los 3 planos están separados en bloques de 0x8000 bytes.
    static const int plane[3] = {
        0 * 0x8000 * 8,
        1 * 0x8000 * 8,
        2 * 0x8000 * 8
    };

    // Cada plano de un tile de 16x16 ocupa exactamente 32 bytes (256 bits).
    const int inc = 16 * 16;

    // El juego tiene exactamente 1024 tiles (0x8000 bytes / 32 bytes)
    g->num_tiles = 1024;

    for (int t = 0; t < g->num_tiles; t++) {
        for (int y = 0; y < 16; y++) {
            // Cada fila de una banda ocupa 1 byte (8 bits)
            int bit_y = y * 8;

            for (int x = 0; x < 16; x++) {
                // Layout clásico de Capcom: 
                // Los píxeles 0-7 están en la primera mitad del stream de bits (0..127)
                // Los píxeles 8-15 están en la segunda mitad, desplazados +16 bytes (+128 bits)
                int bit_x = (x < 8) ? x : (128 + (x - 8));

                int pen = 0;
                for (int p = 0; p < 3; p++) {
                    int b = plane[p] + t * inc + bit_y + bit_x;
                    pen |= bit_get_msb(g->til, b) << p;
                }
                g->til_pix[t][y][x] = (uint8_t)pen; // 0..7
            }
        }
    }
}

static void decode_sprites(Gng *g)
{
    // 16x16, 4bpp. La ROM de sprites totaliza 0x18000 bytes.
    // Cada sprite completo ocupa 128 bytes en total (64 bytes por cada mitad/split de planos).
    g->num_sprites = (int)(GNG_SPR_ROM_SIZE / 0x80); // 0x18000 / 128 = 768 sprites nativos
    if (g->num_sprites > GNG_MAX_SPRITES) g->num_sprites = GNG_MAX_SPRITES;
    if (g->num_sprites < 512) g->num_sprites = 512;

    const int half_offset = GNG_SPR_ROM_SIZE / 2; // El bloque de división (0x0C000 bytes)

    for (int s = 0; s < g->num_sprites; s++) {
        for (int y = 0; y < 16; y++) {
            int y_block = y >> 3; // 0 para la mitad superior (filas 0..7), 1 para la inferior (8..15)
            int y_sub   = y & 7;  // Línea local de 0 a 7 dentro de su bloque vertical

            for (int x = 0; x < 16; x++) {
                int x_block = x >> 2; // Bloques de 4 píxeles: 0 (cols 0..3), 1 (4..7), 2 (8..11), 3 (12..15)
                int x_sub   = x & 3;  // Píxel local de 0 a 3 dentro del grupo horizontal de 4

                // Cálculo del índice exacto de byte usando la estructura entrelazada de Capcom
                int byte_idx = (s * 64) + (y_block * 32) + (x_block * 8) + y_sub;

                // El hardware de Capcom divide el bitstream en dos bloques simétricos en la ROM
                uint8_t hi_byte = g->spr[byte_idx];               // Contiene los planos bajos de MAME
                uint8_t lo_byte = g->spr[byte_idx + half_offset]; // Contiene los planos altos de MAME

                // Desplazamiento de bits según nible superior (7..4) o inferior (3..0)
                int bit_upper = 7 - x_sub;
                int bit_lower = 3 - x_sub;

                // Extraemos los planos respetando estrictamente la precedencia nativa de MAME:
                // MAME plane[0] (MSB, Bit 3) -> lo_byte nible inferior
                // MAME plane[1] (Bit 2)      -> lo_byte nible superior
                // MAME plane[2] (Bit 1)      -> hi_byte nible inferior
                // MAME plane[3] (LSB, Bit 0) -> hi_byte nible superior
                int b3 = (lo_byte >> bit_lower) & 1;
                // ...
                int b2 = (lo_byte >> bit_upper) & 1;
                int b1 = (hi_byte >> bit_lower) & 1;
                int b0 = (hi_byte >> bit_upper) & 1;

                // Empaquetamos el color final de 4 bits (0..15)
                g->spr_pix[s][y][x] = (uint8_t)((b3 << 3) | (b2 << 2) | (b1 << 1) | b0);
            }
        }
    }
}


// =============================================================================
// Bus 6809 (según mapa MAME) [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
// =============================================================================
static uint8_t gng_read_port(Gng *g, uint16_t addr)
{
    switch (addr) {
        case 0x3000: return g->in_system;
        case 0x3001: return g->in_p1;
        case 0x3002: return g->in_p2;
        case 0x3003: return g->dsw1;
        case 0x3004: return g->dsw2;
        default: return 0xFF;
    }
}

static mc6809byte__t cpu_read(mc6809__t *cpu, mc6809addr__t addr, bool ifetch)
{
    (void)ifetch;
    Gng *g = (Gng*)cpu->user;

    if (addr <= 0x1dff) return g->ram[addr];
    if (addr >= 0x1e00 && addr <= 0x1fff) return g->sprram[addr - 0x1e00];

    if (addr >= 0x2000 && addr <= 0x27ff) return g->fgvram[addr - 0x2000];
    if (addr >= 0x2800 && addr <= 0x2fff) return g->bgvram[addr - 0x2800];

    if (addr >= 0x3000 && addr <= 0x3004) return gng_read_port(g, addr);

    if (addr >= 0x4000 && addr <= 0x5fff) return g->bank_ptr[addr - 0x4000];

    if (addr >= 0x6000) return g->mainrom[addr];

    return 0xFF;
}

static void set_bank(Gng *g, uint8_t data)
{
    // MAME: if data==4 -> entry 4 else entry (data&3) [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
    uint8_t b = (data == 4) ? 4 : (data & 3);
    g->bank = b;

    if (b == 4) g->bank_ptr = &g->mainrom[0x4000];
    else        g->bank_ptr = &g->mainrom[0x10000 + (b * 0x2000)];
}

static void cpu_write(mc6809__t *cpu, mc6809addr__t addr, mc6809byte__t val)
{
    Gng *g = (Gng*)cpu->user;

    if (addr <= 0x1dff) { g->ram[addr] = (uint8_t)val; return; }
    if (addr >= 0x1e00 && addr <= 0x1fff) { g->sprram[addr - 0x1e00] = (uint8_t)val; return; }

    if (addr >= 0x2000 && addr <= 0x27ff) { g->fgvram[addr - 0x2000] = (uint8_t)val; return; }
    if (addr >= 0x2800 && addr <= 0x2fff) { g->bgvram[addr - 0x2800] = (uint8_t)val; return; }

    // Palette split HI/LO [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
    if (addr >= 0x3800 && addr <= 0x38ff) { g->pal_ext[addr - 0x3800] = (uint8_t)val; palette_recalc(g); return; }
    if (addr >= 0x3900 && addr <= 0x39ff) { g->pal[addr - 0x3900] = (uint8_t)val; palette_recalc(g); return; }

    if (addr == 0x3a00) { g->sound_latch = (uint8_t)val; return; }

    if (addr == 0x3b08) { g->scrollx[0] = (uint8_t)val; return; }
    if (addr == 0x3b09) { g->scrollx[1] = (uint8_t)val; return; }
    if (addr == 0x3b0a) { g->scrolly[0] = (uint8_t)val; return; }
    if (addr == 0x3b0b) { g->scrolly[1] = (uint8_t)val; return; }

    // En MAME 0x3c00 es NOP / DMA trigger (buffer sprite). Nosotros lo usamos como “buffer”. [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
    if (addr == 0x3c00) {
        (void)val;
        memcpy(g->sprbuf, g->sprram, sizeof(g->sprbuf));
        return;
    }

    // flipscreen / coin counters en 0x3d00... [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
    if (addr >= 0x3d00 && addr <= 0x3d07) {
        (void)val;
        return;
    }

    if (addr == 0x3e00) { set_bank(g, (uint8_t)val); return; }
}

static void cpu_fault(mc6809__t *cpu, mc6809fault__t fault)
{
    fprintf(stderr, "[MC6809] Fault %d en PC=$%04X\n", fault, (unsigned)cpu->instpc);
}

// =============================================================================
// Render (prioridad como MAME: BG back -> sprites -> BG front -> FG) [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
// =============================================================================
static inline void put_px(Gng *g, int x, int y, uint32_t c)
{
    if ((unsigned)x < GNG_W && (unsigned)y < GNG_H) g->fb[y * GNG_W + x] = c;
}

static inline bool dips_flipscreen(const Gng *g)
{
    // DSW1 bit7: Flip Screen (0=On, 1=Off) [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
    return (g->dsw1 & 0x80) == 0;
}

static uint8_t fg_pen_at(Gng *g, int map_x, int map_y, uint8_t *color_out)
{
    map_x &= 0xFF;
    map_y &= 0xFF;

    int col = (map_x >> 3) & 31;
    int row = (map_y >> 3) & 31;
    int idx = row * 32 + col; // scan rows [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)

    uint8_t codeL = g->fgvram[idx];
    uint8_t attr  = g->fgvram[idx + 0x400];

    int code = (int)codeL + ((attr & 0xC0) << 2); // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
    int fl   = (attr & 0x30) >> 4;                // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
    uint8_t color = attr & 0x0F;                  // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)

    int px = map_x & 7;
    int py = map_y & 7;
    if ((fl & 1)) px = 7 - px;
    if ((fl & 2)) py = 7 - py;

    if (color_out) *color_out = color;
    return g->chr_pix[code & (g->num_chars - 1)][py][px];
}

static uint8_t bg_pen_at(Gng *g, int map_x, int map_y, uint8_t *color_out, int *group_out)
{
    // Forzado limpio a positivo para evitar bugs de signo en el bitmask
    unsigned int mx = (unsigned int)map_x & 0x1FF;
    unsigned int my = (unsigned int)map_y & 0x1FF;

    int col = (int)(mx >> 4) & 31;
    int row = (int)(my >> 4) & 31;
    int idx = col * 32 + row; // TILEMAP_SCAN_COLS

    uint8_t codeL = g->bgvram[idx];
    uint8_t attr  = g->bgvram[idx + 0x400];

    int code = (int)codeL + ((attr & 0xC0) << 2);
    int fl   = (attr & 0x30) >> 4;               
    uint8_t color = attr & 0x07;                 
    int group = (attr & 0x08) >> 3;              

    int px = (int)(mx & 15);
    int py = (int)(my & 15);
    if (fl & 1) px = 15 - px;
    if (fl & 2) py = 15 - py;

    if (color_out) *color_out = color;
    if (group_out) *group_out = group;

    return g->til_pix[code & (GNG_MAX_TILES - 1)][py][px];
}

static void draw_bg(Gng *g, bool front_half)
{
    // El scroll del hardware original junta el byte bajo y el byte alto
    int scrollx = (int)g->scrollx[0] + 256 * (int)g->scrollx[1];
    int scrolly = (int)g->scrolly[0] + 256 * (int)g->scrolly[1];

    for (int y = 0; y < GNG_H; y++) {
        for (int x = 0; x < GNG_W; x++) {
            // Aplicar scrolls y offsets nativos del driver arcade
            int map_x = x + scrollx + GNG_SCROLL_DX;
			int map_y = y + scrolly + GNG_SCROLL_DY;

            uint8_t color;
            int group;
            uint8_t pen = bg_pen_at(g, map_x, map_y, &color, &group);

            // Control de prioridades exacto (Transmask de MAME)
            uint32_t transmask = (group == 0) ? 0x00 : 0x18;
            if (front_half) {
                if ((transmask & (1 << pen)) == 0) continue;
            } else {
                if ((transmask & (1 << pen)) != 0) continue;
            }

            // BG tile colour lookup:
            //   índice = GNG_PAL_BG_BASE + grupo_color*8 + pen
            //   (base 0x00, 8 grupos × 8 pens = 0x00-0x3F)
            uint8_t palidx = (uint8_t)(GNG_PAL_BG_BASE + (color * 8) + (pen & 7));
            put_px(g, x, y, g->argb[palidx]);
        }
    }
}

static void draw_fg(Gng *g)
{
    for (int y = 0; y < GNG_H; y++) {
        for (int x = 0; x < GNG_W; x++) {
            
			int map_x = x + GNG_SCROLL_DX;
			int map_y = y + GNG_SCROLL_DY;

            uint8_t color;
            uint8_t pen = fg_pen_at(g, map_x, map_y, &color);

            if (pen == 3) continue; // FG transparent pen=3 [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)

            // FG char colour lookup:
            //   índice = GNG_PAL_FG_BASE + grupo_color*4 + pen
            //   (base 0x80, 16 grupos × 4 pens = 0x80-0xBF)
            uint8_t palidx = (uint8_t)(GNG_PAL_FG_BASE + (color * 4) + (pen & 3));
            put_px(g, x, y, g->argb[palidx]);
        }
    }
}

static void draw_sprites(Gng *g)
{
    bool flip = dips_flipscreen(g);

    for (int offs = (int)sizeof(g->sprbuf) - 4; offs >= 0; offs -= 4) {
        uint8_t attributes = g->sprbuf[offs + 1];

        int sx = (int)g->sprbuf[offs + 3] - 0x100 * (attributes & 0x01); // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
        int sy = (int)g->sprbuf[offs + 2];
        int flipx = (attributes & 0x04) != 0;                            // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
        int flipy = (attributes & 0x08) != 0;                            // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)

        if (flip) {
            sx = 240 - sx;
            sy = 240 - sy;
            flipx = !flipx;
            flipy = !flipy;
        }

        int code  = (int)g->sprbuf[offs] + ((attributes << 2) & 0x300);   // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
        int color = (attributes >> 4) & 3;                               // [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)

        sx += GNG_SCROLL_DX; // sprite offset +128/+6 [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
        sy += GNG_SCROLL_DY;

        code %= g->num_sprites;

        for (int y = 0; y < 16; y++) {
            int yy = flipy ? (15 - y) : y;
            int py = sy + y;
            if ((unsigned)py >= GNG_H) continue;

            for (int x = 0; x < 16; x++) {
                int xx = flipx ? (15 - x) : x;
                int px = sx + x;
                if ((unsigned)px >= GNG_W) continue;

                uint8_t pen = g->spr_pix[code][yy][xx];
                if (pen == 15) continue; // transpen=15 [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)

                // Sprite colour lookup:
                //   índice = GNG_PAL_SPR_BASE + grupo_color*16 + pen
                //   (base 0x40, 4 grupos × 16 pens = 0x40-0x7F)
                uint8_t palidx = (uint8_t)(GNG_PAL_SPR_BASE + color * 16 + (pen & 15));
                put_px(g, px, py, g->argb[palidx]);
            }
        }
    }
}

// =============================================================================
// API
// =============================================================================
void gng_init(Gng *g)
{
    memset(g, 0, sizeof(*g));

    g->in_system = 0xFF;
    g->in_p1     = 0xFF;
    g->in_p2     = 0xFF;

    // DIPs por defecto
    g->dsw1 = 0xFF;
    g->dsw2 = 0xFF;

    memset(&g->cpu, 0, sizeof(g->cpu));
    g->cpu.read  = cpu_read;
    g->cpu.write = cpu_write;
    g->cpu.fault = cpu_fault;
    g->cpu.user  = g;

    g->bank_ptr = &g->mainrom[0x4000];
    g->bank = 4;
}

void gng_destroy(Gng *g)
{
    free(g->chr_pix);
    free(g->til_pix);
    free(g->spr_pix);

    if (g->audio_dev) SDL_CloseAudioDevice(g->audio_dev);
    if (g->texture) SDL_DestroyTexture(g->texture);
    if (g->renderer) SDL_DestroyRenderer(g->renderer);
    if (g->window) SDL_DestroyWindow(g->window);
    SDL_Quit();
}

int gng_load_roms(Gng *g, const char *dir)
{
    memset(g->mainrom, 0xFF, sizeof(g->mainrom));
    memset(g->chr, 0x00, sizeof(g->chr));
    memset(g->til, 0x00, sizeof(g->til));
    memset(g->spr, 0x00, sizeof(g->spr));

    // --- maincpu: gg4 (16k @4000), gg3 (32k @8000), gg5 (32k @10000)
    const char *r04[] = { "gg4.bin", "mmt04d.10n", "mm_c_04" };
    const char *r03[] = { "gg3.bin", "mmt03d.8n",  "mm_c_03" };
    const char *r05[] = { "gg5.bin", "mmt05d.13n", "mm_c_05" };

    int e = 0;
    e |= load_rom_any(dir, r04, 3, g->mainrom, 0x4000, sizeof(g->mainrom), 0x4000);
    e |= load_rom_any(dir, r03, 3, g->mainrom, 0x8000, sizeof(g->mainrom), 0x8000);
    e |= load_rom_any(dir, r05, 3, g->mainrom, 0x10000, sizeof(g->mainrom), 0x8000);

    // --- chars
    const char *c01[] = { "gg1.bin", "mm01.11e" };
    e |= load_rom_any(dir, c01, 2, g->chr, 0x0000, sizeof(g->chr), 0x4000);

    // --- tiles: gg11..gg6
    const char *t11[] = { "gg11.bin", "mm11.3e" };
    const char *t10[] = { "gg10.bin", "mm10.1e" };
    const char *t09[] = { "gg9.bin",  "mm09.3c" };
    const char *t08[] = { "gg8.bin",  "mm08.1c" };
    const char *t07[] = { "gg7.bin",  "mm07.3b" };
    const char *t06[] = { "gg6.bin",  "mm06.1b" };

    e |= load_rom_any(dir, t11, 2, g->til, 0x00000, sizeof(g->til), 0x4000);
    e |= load_rom_any(dir, t10, 2, g->til, 0x04000, sizeof(g->til), 0x4000);
    e |= load_rom_any(dir, t09, 2, g->til, 0x08000, sizeof(g->til), 0x4000);
    e |= load_rom_any(dir, t08, 2, g->til, 0x0C000, sizeof(g->til), 0x4000);
    e |= load_rom_any(dir, t07, 2, g->til, 0x10000, sizeof(g->til), 0x4000);
    e |= load_rom_any(dir, t06, 2, g->til, 0x14000, sizeof(g->til), 0x4000);

    // --- sprites: gg17..gg12
    const char *s17[] = { "gg17.bin", "mm17.4n" };
    const char *s16[] = { "gg16.bin", "mm16.3n" };
    const char *s15[] = { "gg15.bin", "mm15.1n" };
    const char *s14[] = { "gg14.bin", "mm14.4l" };
    const char *s13[] = { "gg13.bin", "mm13.3l" };
    const char *s12[] = { "gg12.bin", "mm12.1l" };

    e |= load_rom_any(dir, s17, 2, g->spr, 0x00000, sizeof(g->spr), 0x4000);
    e |= load_rom_any(dir, s16, 2, g->spr, 0x04000, sizeof(g->spr), 0x4000);
    e |= load_rom_any(dir, s15, 2, g->spr, 0x08000, sizeof(g->spr), 0x4000);
    e |= load_rom_any(dir, s14, 2, g->spr, 0x0C000, sizeof(g->spr), 0x4000);
    e |= load_rom_any(dir, s13, 2, g->spr, 0x10000, sizeof(g->spr), 0x4000);
    e |= load_rom_any(dir, s12, 2, g->spr, 0x14000, sizeof(g->spr), 0x4000);

    if (e) return -1;

    g->chr_pix = (GngCharPix*)calloc(GNG_MAX_CHARS, sizeof(GngCharPix));
    g->til_pix = (GngTilePix*)calloc(GNG_MAX_TILES, sizeof(GngTilePix));
    g->spr_pix = (GngSprPix*) calloc(GNG_MAX_SPRITES, sizeof(GngSprPix));

    if (!g->chr_pix || !g->til_pix || !g->spr_pix) {
        fprintf(stderr, "[GNG] OOM decodificando GFX\n");
        return -1;
    }

    decode_chars(g);
    decode_tiles(g);
    decode_sprites(g);

    memset(g->pal, 0, sizeof(g->pal));
    memset(g->pal_ext, 0, sizeof(g->pal_ext));
    palette_recalc(g);

    set_bank(g, 4);

    printf("[GNG] ROMs OK en '%s' (chars=%d tiles=%d sprites=%d)\n",
           dir, g->num_chars, g->num_tiles, g->num_sprites);
    return 0;
}

void gng_run_frame(Gng *g)
{
    const int cycles_per_frame = (int)((float)GNG_CPU_CLOCK / GNG_FPS);
    const int slices = 16;
    const int cyc_slice = cycles_per_frame / slices;

    for (int sl = 0; sl < slices; sl++) {
        int ran = 0;
        while (ran < cyc_slice) {
            unsigned long before = g->cpu.cycles;
            mc6809_step(&g->cpu);
            ran += (int)(g->cpu.cycles - before);
        }
    }

    // IRQ fin de frame (simplificado)
    g->cpu.irq = true;
    mc6809_step(&g->cpu);
    g->cpu.irq = false;

    for (int i = 0; i < GNG_W * GNG_H; i++) g->fb[i] = 0xFF000000u;

    // Orden MAME de prioridad [3](https://wiki.mamedev.org/index.php/MAME_0.37b7)
    draw_bg(g, false);
    draw_sprites(g);
    draw_bg(g, true);
    draw_fg(g);
}

void gng_render(Gng *g)
{
    SDL_UpdateTexture(g->texture, NULL, g->fb, GNG_W * (int)sizeof(uint32_t));
    SDL_RenderClear(g->renderer);
    SDL_Rect dst = { 0, 0, GNG_W * GNG_SCALE, GNG_H * GNG_SCALE };
    SDL_RenderCopy(g->renderer, g->texture, NULL, &dst);
    SDL_RenderPresent(g->renderer);
}

// =============================================================================
// Inputs (activo LOW) según MAME [2](https://www.sec.gov/Archives/edgar/data/1226616/0000950170-25-059330.txt)
// =============================================================================
static void set_bit_low(uint8_t *reg, uint8_t mask, bool pressed)
{
    if (pressed) *reg &= (uint8_t)~mask;
    else         *reg |= mask;
}

void gng_handle_key(Gng *g, SDL_Scancode sc, bool dn)
{
    switch (sc) {
        case SDL_SCANCODE_1: set_bit_low(&g->in_system, 0x01, dn); break;
        case SDL_SCANCODE_2: set_bit_low(&g->in_system, 0x02, dn); break;
        case SDL_SCANCODE_F1:set_bit_low(&g->in_system, 0x20, dn); break;
        case SDL_SCANCODE_5: set_bit_low(&g->in_system, 0x40, dn); break;
        case SDL_SCANCODE_6: set_bit_low(&g->in_system, 0x80, dn); break;

        case SDL_SCANCODE_RIGHT: set_bit_low(&g->in_p1, 0x01, dn); break;
        case SDL_SCANCODE_LEFT:  set_bit_low(&g->in_p1, 0x02, dn); break;
        case SDL_SCANCODE_DOWN:  set_bit_low(&g->in_p1, 0x04, dn); break;
        case SDL_SCANCODE_UP:    set_bit_low(&g->in_p1, 0x08, dn); break;
        case SDL_SCANCODE_Z:     set_bit_low(&g->in_p1, 0x10, dn); break;
        case SDL_SCANCODE_X:     set_bit_low(&g->in_p1, 0x20, dn); break;

        case SDL_SCANCODE_D: set_bit_low(&g->in_p2, 0x01, dn); break;
        case SDL_SCANCODE_A: set_bit_low(&g->in_p2, 0x02, dn); break;
        case SDL_SCANCODE_S: set_bit_low(&g->in_p2, 0x04, dn); break;
        case SDL_SCANCODE_W: set_bit_low(&g->in_p2, 0x08, dn); break;
        case SDL_SCANCODE_Q: set_bit_low(&g->in_p2, 0x10, dn); break;
        case SDL_SCANCODE_E: set_bit_low(&g->in_p2, 0x20, dn); break;

        case SDL_SCANCODE_F2:
            if (dn) {
                g->turbo_mode = !g->turbo_mode;
                printf("[EMU] Velocidad %s\n", g->turbo_mode ? "MAXIMA" : "normal");
            }
            break;

        case SDL_SCANCODE_ESCAPE:
            if (dn) g->quit = true;
            break;

        default: break;
    }
}

static void print_usage(const char *exe)
{
    printf("Uso: %s <rom_dir>\n", exe);
    printf("Emulador Ghosts'n Goblins (gng) - SDL2\n");
    printf("Teclas:\n");
    printf("  Flechas + Z/X    P1\n");
    printf("  WASD + Q/E       P2\n");
    printf("  1/2              Start\n");
    printf("  5/6              Coin\n");
    printf("  F1               Service\n");
    printf("  F2               Turbo\n");
    printf("  ESC              Salir\n");
}

int main(int argc, char **argv)
{
    const char *rom_dir = (argc > 1) ? argv[1] : ".";

    Gng g;
    gng_init(&g);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    if (gng_load_roms(&g, rom_dir) != 0) {
        fprintf(stderr, "[GNG] Error cargando ROMs\n");
        print_usage(argv[0]);
        gng_destroy(&g);
        return 1;
    }

    g.window = SDL_CreateWindow("Ghosts'n Goblins (gng) - SDL2",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                GNG_W * GNG_SCALE, GNG_H * GNG_SCALE, 0);
    g.renderer = SDL_CreateRenderer(g.window, -1, SDL_RENDERER_ACCELERATED);
    g.texture  = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, GNG_W, GNG_H);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = GNG_AUDIO_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = GNG_AUDIO_SAMPLES;
    want.callback = audio_cb;
    want.userdata = &g;

    g.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g.audio_dev) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice fallo: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(g.audio_dev, 0);
        printf("[AUDIO] OK freq=%d ch=%d (stub)\n", have.freq, have.channels);
    }

    mc6809_reset(&g.cpu);

    uint32_t last = SDL_GetTicks();
    const float ms_per_frame = 1000.0f / GNG_FPS;

    while (!g.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g.quit = true;
            if (ev.type == SDL_KEYDOWN) gng_handle_key(&g, ev.key.keysym.scancode, true);
            if (ev.type == SDL_KEYUP)   gng_handle_key(&g, ev.key.keysym.scancode, false);
        }

        gng_run_frame(&g);
        gng_render(&g);

        uint32_t now = SDL_GetTicks();
        float elapsed = (float)(now - last);

        if (!g.turbo_mode) {
            if (elapsed < ms_per_frame) SDL_Delay((uint32_t)(ms_per_frame - elapsed));
        }
        last = SDL_GetTicks();
    }

    gng_destroy(&g);
    return 0;
}