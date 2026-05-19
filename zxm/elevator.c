#include "elevator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t ea_layer_prom[256] = {
    0x08,0x09,0x08,0x0A,0x00,0x05,0x00,0x0F,0x08,0x09,0x08,0x0A,0x00,0x05,0x00,0x0F,
    0x08,0x09,0x08,0x0B,0x00,0x0D,0x00,0x0F,0x08,0x09,0x08,0x0A,0x00,0x05,0x00,0x0F,
    0x08,0x0A,0x08,0x0A,0x04,0x05,0x00,0x0F,0x08,0x0A,0x08,0x0A,0x04,0x05,0x00,0x0F,
    0x08,0x0A,0x08,0x0A,0x04,0x07,0x0C,0x0F,0x08,0x0A,0x08,0x0A,0x04,0x05,0x00,0x0F,
    0x08,0x0B,0x08,0x0B,0x0C,0x0F,0x0C,0x0F,0x08,0x09,0x08,0x0A,0x00,0x05,0x00,0x0F,
    0x08,0x0B,0x08,0x0B,0x0C,0x0F,0x0C,0x0F,0x08,0x0A,0x08,0x0A,0x04,0x05,0x00,0x0F,
    0x0D,0x0D,0x0C,0x0E,0x0D,0x0D,0x0C,0x0F,0x01,0x05,0x00,0x0A,0x01,0x05,0x00,0x0F,
    0x0D,0x0D,0x0C,0x0F,0x0D,0x0D,0x0C,0x0F,0x01,0x09,0x00,0x0A,0x01,0x05,0x00,0x0F,
    0x0D,0x0D,0x0E,0x0E,0x0D,0x0D,0x0C,0x0F,0x05,0x05,0x02,0x0A,0x05,0x05,0x00,0x0F,
    0x0D,0x0D,0x0E,0x0E,0x0D,0x0D,0x0F,0x0F,0x05,0x05,0x0A,0x0A,0x05,0x05,0x00,0x0F,
    0x0D,0x0D,0x0F,0x0F,0x0D,0x0D,0x0F,0x0F,0x09,0x09,0x08,0x0A,0x01,0x05,0x00,0x0F,
    0x0D,0x0D,0x0F,0x0F,0x0D,0x0D,0x0F,0x0F,0x09,0x09,0x0A,0x0A,0x05,0x05,0x00,0x0F,
    0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
    0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
    0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
    0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
};

static void compute_draworder(ElevatorAction* e)
{
    for (int i = 0; i < 32; i++) {
        int mask = 0;
        for (int j = 3; j >= 0; j--) {
            int data = ea_layer_prom[0x10 * (i & 0x0F) + mask] & 0x0F;
            if (i & 0x10) data >>= 2;
            data &= 0x03;
            mask |= (1 << data);
            e->draworder[i][j] = (uint8_t)data;
        }
    }
}

static int load_file(uint8_t* dst, int max_size, const char* path, int offset, int size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    if (size <= 0) size = (int)fsz;
    if (offset < 0) offset = 0;
    if (offset + size > max_size) size = max_size - offset;

    size_t rd = fread(dst + offset, 1, (size_t)size, f);
    fclose(f);

    if ((int)rd != size) {
        fprintf(stderr, "[ROM] Lectura parcial '%s' (%d/%d)\n", path, (int)rd, size);
        return -1;
    }
    return 0;
}

int ea_load_mainrom(ElevatorAction* e, const char* path, int offset, int size)
{
    int r = load_file(e->mainrom, EA_MAINROM_SIZE, path, offset, size);
    if (r == 0) e->have_mainrom = true;
    return r;
}

int ea_load_sndrom(ElevatorAction* e, const char* path, int offset, int size)
{
    int r = load_file(e->sndrom, EA_SNDROM_SIZE, path, offset, size);
    if (r == 0) e->have_sndrom = true;
    return r;
}

int ea_load_gfxrom(ElevatorAction* e, const char* path, int offset, int size)
{
    int r = load_file(e->gfxrom, EA_GFXROM_SIZE, path, offset, size);
    if (r == 0) e->have_gfxrom = true;
    return r;
}

static inline uint8_t get_char_set_from_offset(int offset)
{
    return (offset < 0x1800) ? 0 : 1;
}

static void decode_chars_from_charram(ElevatorAction* e, int set)
{
    int base = set ? 0x1800 : 0x0000;

    for (int t = 0; t < EA_CHARS_NUM; t++) {
        for (int row = 0; row < EA_CHAR_H; row++) {
            int off = base + (t * 8) + row;

            uint8_t p0 = e->charram[off + 0x0000];
            uint8_t p1 = e->charram[off + 0x0800];
            uint8_t p2 = e->charram[off + 0x1000];

            for (int col = 0; col < EA_CHAR_W; col++) {
                int bit = 7 - col;
                uint8_t pix =
                    ((p0 >> bit) & 1) |
                    (((p1 >> bit) & 1) << 1) |
                    (((p2 >> bit) & 1) << 2);
                e->chars[set][t][row][col] = pix;
            }
        }
    }
}

static void decode_sprites_from_charram(ElevatorAction* e, int set)
{
    int base = set ? 0x1800 : 0x0000;

    for (int s = 0; s < EA_SPRITES_NUM; s++) {
        int spr_base = base + (s * 32);

        for (int row = 0; row < EA_SPR_H; row++) {
            int ro = row * 2;

            uint8_t p0a = e->charram[spr_base + 0x0000 + ro + 0];
            uint8_t p0b = e->charram[spr_base + 0x0000 + ro + 1];
            uint8_t p1a = e->charram[spr_base + 0x0800 + ro + 0];
            uint8_t p1b = e->charram[spr_base + 0x0800 + ro + 1];
            uint8_t p2a = e->charram[spr_base + 0x1000 + ro + 0];
            uint8_t p2b = e->charram[spr_base + 0x1000 + ro + 1];

            for (int col = 0; col < EA_SPR_W; col++) {
                int bit = 7 - (col & 7);
                uint8_t b0 = (col < 8) ? ((p0a >> bit) & 1) : ((p0b >> bit) & 1);
                uint8_t b1 = (col < 8) ? ((p1a >> bit) & 1) : ((p1b >> bit) & 1);
                uint8_t b2 = (col < 8) ? ((p2a >> bit) & 1) : ((p2b >> bit) & 1);

                e->sprites[set][s][row][col] = b0 | (b1 << 1) | (b2 << 2);
            }
        }
    }
}

static void decode_gfx_if_dirty(ElevatorAction* e)
{
    if (!e->gfx_dirty) return;

    decode_chars_from_charram(e, 0);
    decode_chars_from_charram(e, 1);
    decode_sprites_from_charram(e, 0);
    decode_sprites_from_charram(e, 1);

    e->gfx_dirty = false;
    e->vram_dirty[0] = true;
    e->vram_dirty[1] = true;
    e->vram_dirty[2] = true;
}

static void update_palette_entry(ElevatorAction* e, int idx)
{
    int off = idx * 2;

    uint8_t lo = e->palram[off + 0];
    uint8_t hi = e->palram[off + 1];

    int r, g, b;
    int bit0, bit1, bit2;

    bit0 = ((~hi) >> 6) & 1;
    bit1 = ((~hi) >> 7) & 1;
    bit2 = ((~lo) >> 0) & 1;
    r = bit0 * 0x21 + bit1 * 0x47 + bit2 * 0x97;

    bit0 = ((~hi) >> 3) & 1;
    bit1 = ((~hi) >> 4) & 1;
    bit2 = ((~hi) >> 5) & 1;
    g = bit0 * 0x21 + bit1 * 0x47 + bit2 * 0x97;

    bit0 = ((~hi) >> 0) & 1;
    bit1 = ((~hi) >> 1) & 1;
    bit2 = ((~hi) >> 2) & 1;
    b = bit0 * 0x21 + bit1 * 0x47 + bit2 * 0x97;

    e->palette[idx & 63] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void rebuild_palette(ElevatorAction* e)
{
    for (int i = 0; i < EA_TOTAL_COLORS; i++) update_palette_entry(e, i);
    e->palette_dirty = false;
}

static inline uint32_t pen_color(ElevatorAction* e, int colorbank, int pix)
{
    if (pix == 0) return 0;
    int idx = ((colorbank & 7) * 8 + (pix & 7)) & 63;
    return e->palette[idx];
}

static inline bool flip_x(const ElevatorAction* e) { return (e->video_mode & 0x01) != 0; }
static inline bool flip_y(const ElevatorAction* e) { return (e->video_mode & 0x02) != 0; }
static inline int sprite_page_off(const ElevatorAction* e) { return (e->video_mode & 0x04) ? 0x80 : 0x00; }
static inline bool sprites_on(const ElevatorAction* e) { return (e->video_mode & 0x80) != 0; }

static void draw_char_to_plane(ElevatorAction* e, int plane, int tile, int set, int colorbank, int sx, int sy)
{
    tile &= 0xFF;
    sx &= 255;
    sy &= 255;

    bool fx = /*flip_x(e)*/ true;
    bool fy = flip_y(e);

    for (int y = 0; y < EA_CHAR_H; y++) {
        int yy = fy ? (EA_CHAR_H - 1 - y) : y;
        int dy = (sy + y) & 255;

        for (int x = 0; x < EA_CHAR_W; x++) {
            int xx = fx ? (EA_CHAR_W - 1 - x) : x;
            int dx = (sx + x) & 255;

            uint8_t pix = e->chars[set][tile][yy][xx];
            uint32_t c = pen_color(e, colorbank, pix);
            if (c) e->planebuf[plane][dy * EA_LOG_W + dx] = c;
        }
    }
}

static int layer_colorbank(ElevatorAction* e, int layer)
{
    if (layer == 0) return (e->colorbank_12 >> 0) & 7;
    if (layer == 1) return (e->colorbank_12 >> 4) & 7;
    return (e->colorbank_3s >> 0) & 7;
}

static int layer_charbank(ElevatorAction* e, int layer)
{
    if (layer == 0) return (e->colorbank_12 & 0x08) ? 1 : 0;
    if (layer == 1) return (e->colorbank_12 & 0x80) ? 1 : 0;
    return (e->colorbank_3s & 0x08) ? 1 : 0;
}

static int sprite_colorbank(ElevatorAction* e)
{
    return (e->colorbank_3s >> 4) & 3;
}

static void update_plane(ElevatorAction* e, int layer)
{
    if (!e->vram_dirty[layer] && !e->scroll_dirty && !e->palette_dirty) return;

    memset(e->planebuf[layer], 0, sizeof(e->planebuf[layer]));

    int colorbank = layer_colorbank(e, layer);
    int set = layer_charbank(e, layer);

    for (int i = 0; i < EA_VRAM_SIZE; i++) {
        int tx = i & 31;
        int ty = i >> 5;

        uint8_t code = e->vram[layer][i];

        draw_char_to_plane(e, layer, code, set, colorbank, tx * 8, ty * 8);
    }

    e->vram_dirty[layer] = false;
}

static int layer_enable_mask(int layer)
{
    if (layer == 0) return 0x10;
    if (layer == 1) return 0x20;
    return 0x40;
}

static void blit_plane(ElevatorAction* e, int layer, uint32_t* dst)
{
    if ((e->video_mode & layer_enable_mask(layer)) == 0) {
        memset(dst, 0, EA_SCREEN_W * EA_SCREEN_H * sizeof(uint32_t));
        return;
    }

    int col_base = layer * 32;

    uint8_t hscr = e->scroll_h[layer];
    uint8_t vscr = e->scroll_v[layer];

    for (int y = 0; y < EA_SCREEN_H; y++) {
        int ly = y + EA_VIS_Y0;
        for (int x = 0; x < EA_SCREEN_W; x++) {
            int col = (x >> 3) & 31;
            int8_t scy = (int8_t)e->colscroll[col_base + col];

            int sx = (x + (int)hscr) & 255;
            int sy = (ly + (int)vscr + (int)scy) & 255;

            dst[y * EA_SCREEN_W + x] = e->planebuf[layer][sy * EA_LOG_W + sx];
        }
    }
}

static void draw_sprites(ElevatorAction* e, uint32_t* dst)
{
    memset(dst, 0, EA_SCREEN_W * EA_SCREEN_H * sizeof(uint32_t));
    if (!sprites_on(e)) return;

    int page = sprite_page_off(e);
    int scol = sprite_colorbank(e);

    for (int which = 0; which < 32; which++) {
        int o = page + which * 4;

        int sx = (int)e->spriteram[o + 0] - 1;
        int sy = 240 - (int)e->spriteram[o + 1];

        uint8_t attr = e->spriteram[o + 2];
        uint8_t code = e->spriteram[o + 3];

        int set = (code & 0x40) ? 1 : 0;
        int tile = code & 0x3F;

        bool fx = (attr & 0x01) != 0;
        bool fy = (attr & 0x02) != 0;

        bool gfx_fx = /*flip_x(e) ? !fx :*/ fx;
        bool gfx_fy = flip_y(e) ? !fy : fy;

        int dy0 = sy - EA_VIS_Y0;

        for (int y = 0; y < EA_SPR_H; y++) {
            int yy = gfx_fy ? (EA_SPR_H - 1 - y) : y;
            int dy = dy0 + y;
            if (dy < 0 || dy >= EA_SCREEN_H) continue;

            for (int x = 0; x < EA_SPR_W; x++) {
                int xx = gfx_fx ? (EA_SPR_W - 1 - x) : x;
                int dx = (sx + x) & 255;
                if (dx < 0 || dx >= EA_SCREEN_W) continue;

                uint8_t pix = e->sprites[set][tile][yy][xx];
                if (pix == 0) continue;

                uint32_t c = pen_color(e, scol, pix);
                if (c) dst[dy * EA_SCREEN_W + dx] = c;
            }
        }
    }
}

static void compose_frame(ElevatorAction* e)
{
    decode_gfx_if_dirty(e);
    if (e->palette_dirty) rebuild_palette(e);

    update_plane(e, 0);
    update_plane(e, 1);
    update_plane(e, 2);

    blit_plane(e, 0, e->layerbuf[1]);
    blit_plane(e, 1, e->layerbuf[2]);
    blit_plane(e, 2, e->layerbuf[3]);

    draw_sprites(e, e->layerbuf[0]);

    uint8_t pri = e->pri_reg & 31;

    for (int i = 0; i < EA_SCREEN_W * EA_SCREEN_H; i++) {
        uint32_t out = 0xFF000000u;
        for (int j = 0; j < 4; j++) {
            int lid = e->draworder[pri][j] & 3;
            uint32_t p = e->layerbuf[lid][i];
            if (p) out = p;
        }
        e->framebuffer[i] = out;
    }

    e->scroll_dirty = false;
}

static uint8_t gfxrom_read_and_inc(ElevatorAction* e)
{
    uint16_t offs = (uint16_t)e->exrom_lo | ((uint16_t)e->exrom_hi << 8);
    uint8_t ret = 0x00;

    if (e->have_gfxrom && offs < EA_GFXROM_SIZE) ret = e->gfxrom[offs];

    offs++;
    e->exrom_lo = (uint8_t)(offs & 0xFF);
    e->exrom_hi = (uint8_t)(offs >> 8);

    return ret;
}

static uint8_t main_read(void* ud, uint16_t addr)
{
    ElevatorAction* e = (ElevatorAction*)ud;

    if (addr < 0x8000) return e->mainrom[addr];

    if (addr >= 0x8000 && addr <= 0x87FF) return e->ram[addr - 0x8000];

    if (addr >= 0x9000 && addr <= 0xBFFF) return e->charram[addr - 0x9000];

    if (addr >= 0xC000 && addr <= 0xC3FF) return e->vram_coll[addr - 0xC000];

    if (addr >= 0xC400 && addr <= 0xC7FF) return e->vram[0][addr - 0xC400];
    if (addr >= 0xC800 && addr <= 0xCBFF) return e->vram[1][addr - 0xC800];
    if (addr >= 0xCC00 && addr <= 0xCFFF) return e->vram[2][addr - 0xCC00];

    if (addr >= 0xD000 && addr <= 0xD05F) return e->colscroll[addr - 0xD000];

    if (addr >= 0xD100 && addr <= 0xD1FF) return e->spriteram[addr - 0xD100];

    if (addr >= 0xD200 && addr <= 0xD27F) return e->palram[addr - 0xD200];

    if (addr == 0xD300) return e->pri_reg;

    if (addr == 0xD404) return gfxrom_read_and_inc(e);

    return 0xFF;
}

static void main_write(void* ud, uint16_t addr, uint8_t val)
{
    ElevatorAction* e = (ElevatorAction*)ud;

    if (addr >= 0x8000 && addr <= 0x87FF) {
        e->ram[addr - 0x8000] = val;
        return;
    }

    if (addr >= 0x9000 && addr <= 0xBFFF) {
        int off = addr - 0x9000;
        if (e->charram[off] != val) {
            e->charram[off] = val;
            e->gfx_dirty = true;
        }
        return;
    }

    if (addr >= 0xC000 && addr <= 0xC3FF) {
        e->vram_coll[addr - 0xC000] = val;
        return;
    }

    if (addr >= 0xC400 && addr <= 0xC7FF) {
        e->vram[0][addr - 0xC400] = val;
        e->vram_dirty[0] = true;
        return;
    }

    if (addr >= 0xC800 && addr <= 0xCBFF) {
        e->vram[1][addr - 0xC800] = val;
        e->vram_dirty[1] = true;
        return;
    }

    if (addr >= 0xCC00 && addr <= 0xCFFF) {
        e->vram[2][addr - 0xCC00] = val;
        e->vram_dirty[2] = true;
        return;
    }

    if (addr >= 0xD000 && addr <= 0xD05F) {
        e->colscroll[addr - 0xD000] = val;
        e->scroll_dirty = true;
        return;
    }

    if (addr >= 0xD100 && addr <= 0xD1FF) {
        e->spriteram[addr - 0xD100] = val;
        return;
    }

    if (addr >= 0xD200 && addr <= 0xD27F) {
        e->palram[addr - 0xD200] = val;
        e->palette_dirty = true;
        return;
    }

    if (addr == 0xD300) {
        e->pri_reg = val & 31;
        return;
    }

    if (addr >= 0xD500 && addr <= 0xD505) {
        int idx = addr - 0xD500;
        int layer = idx / 2;
        if ((idx & 1) == 0) e->scroll_h[layer] = val;
        else e->scroll_v[layer] = val;
        e->scroll_dirty = true;
        return;
    }

    if (addr == 0xD506) {
        e->colorbank_12 = val;
        e->vram_dirty[0] = true;
        e->vram_dirty[1] = true;
        return;
    }

    if (addr == 0xD507) {
        e->colorbank_3s = val;
        e->vram_dirty[2] = true;
        return;
    }

    if (addr == 0xD509) {
        e->exrom_lo = val;
        return;
    }

    if (addr == 0xD50A) {
        e->exrom_hi = val;
        return;
    }

    if (addr == 0xD50B) {
        e->soundlatch = val;
        return;
    }

    if (addr == 0xD600) {
        e->video_mode = val;
        e->scroll_dirty = true;
        return;
    }
}

static uint8_t main_port_in(z80* z, uint16_t p) { (void)z; (void)p; return 0xFF; }
static void main_port_out(z80* z, uint16_t p, uint8_t v) { (void)z; (void)p; (void)v; }

static uint8_t snd_read(void* ud, uint16_t addr)
{
    ElevatorAction* e = (ElevatorAction*)ud;
    if (addr <= 0x3FFF) return e->sndrom[addr];
    if (addr >= 0x4000 && addr <= 0x43FF) return e->sndram[addr - 0x4000];
    if (addr == 0x5000) return e->soundlatch;
    return 0xFF;
}

static void snd_write(void* ud, uint16_t addr, uint8_t val)
{
    ElevatorAction* e = (ElevatorAction*)ud;
    if (addr >= 0x4000 && addr <= 0x43FF) {
        e->sndram[addr - 0x4000] = val;
        return;
    }
    (void)val;
}

static uint8_t snd_port_in(z80* z, uint16_t p) { (void)z; (void)p; return 0xFF; }
static void snd_port_out(z80* z, uint16_t p, uint8_t v) { (void)z; (void)p; (void)v; }

static void print_usage(const char* exe)
{
    printf("Uso: %s --romdir <carpeta>\n", exe);
}

static void join_path(char* out, size_t outsz, const char* dir, const char* file)
{
    size_t dl = strlen(dir);
    snprintf(out, outsz, "%s%s%s", dir, (dl && (dir[dl-1] == '/' || dir[dl-1] == '\\')) ? "" : "/", file);
}

int ea_load_from_dir(ElevatorAction* e, const char* dirpath)
{
    char path[1024];

    const char* main_list[] = {
        "ea-ic69.bin","ea-ic68.bin","ea-ic67.bin","ea-ic66.bin",
        "ea-ic65.bin","ea-ic64.bin","ea-ic55.bin","ea-ic54.bin"
    };

    const char* snd_list[] = { "ea-ic70.bin", "ea-ic71.bin" };

    const char* gfx_list[] = {
        "ea-ic1.bin","ea-ic2.bin","ea-ic3.bin","ea-ic4.bin",
        "ea-ic5.bin","ea-ic6.bin","ea-ic7.bin","ea-ic8.bin"
    };

    int off = 0x0000;
    for (int i = 0; i < (int)(sizeof(main_list)/sizeof(main_list[0])); i++) {
        join_path(path, sizeof(path), dirpath, main_list[i]);
        if (ea_load_mainrom(e, path, off, 0) != 0) return -1;
        off += 0x1000;
    }

    int soff = 0x0000;
    for (int i = 0; i < (int)(sizeof(snd_list)/sizeof(snd_list[0])); i++) {
        join_path(path, sizeof(path), dirpath, snd_list[i]);
        ea_load_sndrom(e, path, soff, 0);
        soff += 0x1000;
    }

    int goff = 0x0000;
    for (int i = 0; i < (int)(sizeof(gfx_list)/sizeof(gfx_list[0])); i++) {
        join_path(path, sizeof(path), dirpath, gfx_list[i]);
        if (ea_load_gfxrom(e, path, goff, 0) != 0) return -1;
        goff += 0x1000;
    }

    return 0;
}

void ea_init(ElevatorAction* e)
{
    memset(e, 0, sizeof(*e));
    e->scale = EA_SCALE;

    e->palette_dirty = true;
    e->gfx_dirty = true;
    e->vram_dirty[0] = true;
    e->vram_dirty[1] = true;
    e->vram_dirty[2] = true;
    e->scroll_dirty = true;

    e->video_mode = 0x80 | 0x10 | 0x20 | 0x40;

    compute_draworder(e);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        exit(1);
    }

    e->window = SDL_CreateWindow("Elevator Action (video)",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 EA_SCREEN_W * e->scale, EA_SCREEN_H * e->scale,
                                 SDL_WINDOW_SHOWN);
    if (!e->window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        exit(1);
    }

    e->renderer = SDL_CreateRenderer(e->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!e->renderer) {
        fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        exit(1);
    }

    e->texture = SDL_CreateTexture(e->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, EA_SCREEN_W, EA_SCREEN_H);
    if (!e->texture) {
        fprintf(stderr, "SDL_CreateTexture error: %s\n", SDL_GetError());
        exit(1);
    }

    z80_init(&e->cpu_main);
    e->cpu_main.read_byte = main_read;
    e->cpu_main.write_byte = main_write;
    e->cpu_main.port_in = main_port_in;
    e->cpu_main.port_out = main_port_out;
    e->cpu_main.userdata = e;
    z80_reset(&e->cpu_main);

    z80_init(&e->cpu_snd);
    e->cpu_snd.read_byte = snd_read;
    e->cpu_snd.write_byte = snd_write;
    e->cpu_snd.port_in = snd_port_in;
    e->cpu_snd.port_out = snd_port_out;
    e->cpu_snd.userdata = e;
    z80_reset(&e->cpu_snd);
}

void ea_destroy(ElevatorAction* e)
{
    if (e->texture) SDL_DestroyTexture(e->texture);
    if (e->renderer) SDL_DestroyRenderer(e->renderer);
    if (e->window) SDL_DestroyWindow(e->window);
    SDL_Quit();
}

void ea_run_frame(ElevatorAction* e)
{
    z80_step_n(&e->cpu_main, (unsigned)EA_CYCLES_PER_FRAME);
    z80_pulse_irq(&e->cpu_main, 0);
    z80_step_n(&e->cpu_snd, (unsigned)EA_SND_CYCLES_PER_FRAME);
}

void ea_render(ElevatorAction* e)
{
    compose_frame(e);

    SDL_UpdateTexture(e->texture, NULL, e->framebuffer, EA_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(e->renderer);
    SDL_RenderCopy(e->renderer, e->texture, NULL, NULL);
    SDL_RenderPresent(e->renderer);
}

int main(int argc, char* argv[])
{
    ElevatorAction e;
    ea_init(&e);

    const char* romdir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--romdir") && i + 1 < argc) romdir = argv[++i];
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            ea_destroy(&e);
            return 0;
        }
    }

    if (!romdir) {
        print_usage(argv[0]);
        ea_destroy(&e);
        return 0;
    }

    if (ea_load_from_dir(&e, romdir) != 0) {
        fprintf(stderr, "Error cargando ROMs desde %s\n", romdir);
        ea_destroy(&e);
        return 1;
    }

    bool running = true;
    uint32_t frame_ms = 1000 / EA_FPS;

    while (running) {
        uint32_t start = SDL_GetTicks();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
        }

        ea_run_frame(&e);
        ea_render(&e);

        uint32_t elapsed = SDL_GetTicks() - start;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    ea_destroy(&e);
    return 0;
}