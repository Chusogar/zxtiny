/*
 * pbaction.c  -  Emulador (mini) de Pinball Action (Tehkan, 1985)
 *
 * Objetivo: replicar la misma filosofía que phoenix.c:
 *   - todo en 1 .c + 1 .h
 *   - cargar ROMs desde una carpeta (--dir)
 *   - emular lo esencial de CPU+memmap+video+inputs
 *   - sonido: stub (captura de writes a AY y command latch) para extender luego
 *
 * Referencias (hardware / mapas / GFX):
 *   - Driver clásico MAME (pbaction.c) con mapa de memoria, entradas y layouts.
 *
 * Compilar (ejemplo):
 *   gcc pbaction.c z80/jgz80/z80.c -o pbaction -lSDL2 -O2
 *
 * Ejecutar:
 *   ./pbaction --dir /ruta/a/roms/pbaction
 */

#include "pbaction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

// ----------------------------------------------------------------------------
// (Opcional) API de interrupciones del core Z80
// ----------------------------------------------------------------------------
// Nota: phoenix no usa IRQ/NMI. Pinball Action sí.
// Como no conocemos a priori la API exacta del core Z80 que uses, este fichero
// soporta tres macros opcionales:
//   -DJGZ80_HAVE_RESET  -> expone: void z80_reset(z80*);
//   -DJGZ80_HAVE_NMI    -> expone: void z80_nmi(z80*);
//   -DJGZ80_HAVE_IRQ    -> expone: void z80_irq(z80*, uint8_t vector);
//
// Si tu core usa nombres distintos, adapta estas declaraciones/wrappers.

#ifdef JGZ80_HAVE_RESET
extern void z80_reset(z80* cpu);
#endif
#ifdef JGZ80_HAVE_NMI
extern void z80_nmi(z80* cpu);
#endif
#ifdef JGZ80_HAVE_IRQ
extern void z80_irq(z80* cpu, uint8_t vector);
#endif

static inline void pb_z80_reset(z80* cpu) {
//#ifdef JGZ80_HAVE_RESET
    z80_reset(cpu);
//#else
//    (void)cpu;
//#endif
}

static inline void pb_z80_pulse_nmi(z80* cpu) {
#ifdef JGZ80_HAVE_NMI
    z80_nmi(cpu);
#else
    //(void)cpu;
	z80_pulse_nmi(cpu);
#endif
}

static inline void pb_z80_irq_vec(z80* cpu, uint8_t vec) {
#ifdef JGZ80_HAVE_IRQ
    z80_irq(cpu, vec);
#else
    (void)cpu; (void)vec;
#endif
}

// ----------------------------------------------------------------------------
// Utilidades de fichero/ROM
// ----------------------------------------------------------------------------

static int file_exists(const char* path) {
    return access(path, R_OK) == 0;
}

static void join_path(char* out, size_t out_sz, const char* dir, const char* name) {
    size_t dl = strlen(dir);
    if (dl > 0 && (dir[dl-1] == '/' || dir[dl-1] == '\\'))
        snprintf(out, out_sz, "%s%s", dir, name);
    else
        snprintf(out, out_sz, "%s/%s", dir, name);
}

static void str_tolower(char* s) {
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static int find_file_ci(const char* dir, const char* base, char* out_path, size_t out_sz) {
    // prueba exacto + extensiones comunes
    const char* exts[] = {"", ".bin", ".rom", ".dat"};
    char tmp[1024];
    for (unsigned i = 0; i < sizeof(exts)/sizeof(exts[0]); i++) {
        char name[256];
        snprintf(name, sizeof(name), "%s%s", base, exts[i]);
        join_path(tmp, sizeof(tmp), dir, name);
        if (file_exists(tmp)) { snprintf(out_path, out_sz, "%s", tmp); return 0; }
    }

    // búsqueda case-insensitive por nombre sin extension
    DIR* d = opendir(dir);
    if (!d) return -1;

    char base_l[256];
    snprintf(base_l, sizeof(base_l), "%s", base);
    str_tolower(base_l);

    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char pref[256];
        snprintf(pref, sizeof(pref), "%s", de->d_name);
        char* dot = strchr(pref, '.');
        if (dot) *dot = 0;
        str_tolower(pref);
        if (strcmp(pref, base_l) == 0) {
            join_path(tmp, sizeof(tmp), dir, de->d_name);
            if (file_exists(tmp)) { snprintf(out_path, out_sz, "%s", tmp); closedir(d); return 0; }
        }
    }

    closedir(d);
    return -1;
}

static int load_file(uint8_t* dst, int max_size, const char* path, int offset, int len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ROM] No se puede abrir '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    int to_read = (len > 0) ? len : (int)fsz;
    if (offset + to_read > max_size) {
        fprintf(stderr, "[ROM] '%s': desbordamiento (off=%d len=%d max=%d)\n", path, offset, to_read, max_size);
        fclose(f);
        return -1;
    }

    int n = (int)fread(dst + offset, 1, (size_t)to_read, f);
    fclose(f);

    if (n != to_read) {
        fprintf(stderr, "[ROM] '%s': leidos %d de %d bytes\n", path, n, to_read);
        return -1;
    }

    printf("[ROM] '%s' -> offset 0x%X, %d bytes\n", path, offset, n);
    return 0;
}

// ----------------------------------------------------------------------------
// Carga desde directorio
// ----------------------------------------------------------------------------

int pbaction_load_from_dir(PBAction* s, const char* dir) {
    char path[1024];

    // maincpu ROMs (set 1):
    //   b-p7.bin (16KB) @ 0x0000
    //   b-n7.bin (16KB) @ 0x4000
    //   b-l7.bin ( 8KB) @ 0x8000
    // (lo restante 0xA000-0xBFFF se deja a 0xFF)
    memset(s->rom, 0xFF, sizeof(s->rom));

    if (find_file_ci(dir, "b-p7", path, sizeof(path)) != 0 && find_file_ci(dir, "b-p7.bin", path, sizeof(path)) != 0) {
        fprintf(stderr, "[ROM] No encuentro b-p7(.bin) en %s\n", dir);
        return -1;
    }
    if (load_file(s->rom, (int)sizeof(s->rom), path, 0x0000, 0x4000) != 0) return -1;

    if (find_file_ci(dir, "b-n7", path, sizeof(path)) != 0) {
        fprintf(stderr, "[ROM] No encuentro b-n7(.bin) en %s\n", dir);
        return -1;
    }
    if (load_file(s->rom, (int)sizeof(s->rom), path, 0x4000, 0x4000) != 0) return -1;

    if (find_file_ci(dir, "b-l7", path, sizeof(path)) != 0) {
        fprintf(stderr, "[ROM] No encuentro b-l7(.bin) en %s\n", dir);
        return -1;
    }
    if (load_file(s->rom, (int)sizeof(s->rom), path, 0x8000, 0x2000) != 0) return -1;

    // audiocpu ROM: a-e3.bin (8KB)
    if (find_file_ci(dir, "a-e3", path, sizeof(path)) != 0) {
        fprintf(stderr, "[ROM] No encuentro a-e3(.bin) en %s\n", dir);
        return -1;
    }

    // En vez de depender de una estructura interna, guardamos el ROM de audio aparte:
    // reutilizamos s->ram temporalmente NO; creamos un buffer local estático?
    // Para simplicidad, añadimos un buffer de audio ROM embebido en PBAction via malloc.
    // (ver abajo: s->audiocpu user data usa pbaction_audio_mem_read)

    // Cargar a buffer 'rom_audio' (al final de este fichero).
    extern uint8_t pbaction_audio_rom[0x2000];
    memset(pbaction_audio_rom, 0xFF, 0x2000);
    if (load_file(pbaction_audio_rom, 0x2000, path, 0x0000, 0x2000) != 0) return -1;

    // fgchars: a-s6,a-s7,a-s8 (8KB c/u)
    const char* fg_names[3] = {"a-s6", "a-s7", "a-s8"};
    for (int i = 0; i < 3; i++) {
        if (find_file_ci(dir, fg_names[i], path, sizeof(path)) != 0) {
            fprintf(stderr, "[ROM] No encuentro %s en %s\n", fg_names[i], dir);
            return -1;
        }
        if (load_file(s->fgchars, (int)sizeof(s->fgchars), path, i * 0x2000, 0x2000) != 0) return -1;
    }

    // bgchars: a-j5,a-j6,a-j7,a-j8 (16KB c/u)
    const char* bg_names[4] = {"a-j5", "a-j6", "a-j7", "a-j8"};
    for (int i = 0; i < 4; i++) {
        if (find_file_ci(dir, bg_names[i], path, sizeof(path)) != 0) {
            fprintf(stderr, "[ROM] No encuentro %s en %s\n", bg_names[i], dir);
            return -1;
        }
        if (load_file(s->bgchars, (int)sizeof(s->bgchars), path, i * 0x4000, 0x4000) != 0) return -1;
    }

    // sprites: b-c7,b-d7,b-f7 (8KB c/u)
    const char* sp_names[3] = {"b-c7", "b-d7", "b-f7"};
    for (int i = 0; i < 3; i++) {
        if (find_file_ci(dir, sp_names[i], path, sizeof(path)) != 0) {
            fprintf(stderr, "[ROM] No encuentro %s en %s\n", sp_names[i], dir);
            return -1;
        }
        if (load_file(s->sprites, (int)sizeof(s->sprites), path, i * 0x2000, 0x2000) != 0) return -1;
    }

    printf("[ROM] Carga completa desde %s\n", dir);
    return 0;
}

// ----------------------------------------------------------------------------
// Memoria CPU audio (buffers)
// ----------------------------------------------------------------------------
// ROM audio 0x0000-0x1fff
uint8_t pbaction_audio_rom[0x2000];
static uint8_t pbaction_audio_ram[0x0800];

// ----------------------------------------------------------------------------
// Callbacks Z80 - main
// ----------------------------------------------------------------------------

static uint8_t main_mem_read(void* user, uint16_t addr) {
    PBAction* s = (PBAction*)user;

    if (addr < 0xC000) {
        return s->rom[addr];
    }

    if (addr >= 0xC000 && addr <= 0xCFFF) {
        return s->ram[addr - 0xC000];
    }

    if (addr >= 0xD000 && addr <= 0xD3FF) return s->videoram_fg[addr - 0xD000];
    if (addr >= 0xD400 && addr <= 0xD7FF) return s->colorram_fg[addr - 0xD400];
    if (addr >= 0xD800 && addr <= 0xDBFF) return s->videoram_bg[addr - 0xD800];
    if (addr >= 0xDC00 && addr <= 0xDFFF) return s->colorram_bg[addr - 0xDC00];

    if (addr >= 0xE000 && addr <= 0xE07F) return s->spriteram[addr - 0xE000];
    if (addr >= 0xE400 && addr <= 0xE5FF) return s->palram[addr - 0xE400];

    // Inputs / control
    if (addr == 0xE600) return s->in_p1;
    if (addr == 0xE601) return s->in_p2;
    if (addr == 0xE602) return s->in_sys;
    if (addr == 0xE604) return s->dsw1;
    if (addr == 0xE605) return s->dsw2;

    // e606: watchdog (readnop)

    return 0xFF;
}

static void main_mem_write(void* user, uint16_t addr, uint8_t val) {
    PBAction* s = (PBAction*)user;

    if (addr >= 0xC000 && addr <= 0xCFFF) {
        s->ram[addr - 0xC000] = val;
        return;
    }

    if (addr >= 0xD000 && addr <= 0xD3FF) { s->videoram_fg[addr - 0xD000] = val; return; }
    if (addr >= 0xD400 && addr <= 0xD7FF) { s->colorram_fg[addr - 0xD400] = val; return; }
    if (addr >= 0xD800 && addr <= 0xDBFF) { s->videoram_bg[addr - 0xD800] = val; return; }
    if (addr >= 0xDC00 && addr <= 0xDFFF) { s->colorram_bg[addr - 0xDC00] = val; return; }

    if (addr >= 0xE000 && addr <= 0xE07F) { s->spriteram[addr - 0xE000] = val; return; }

    if (addr >= 0xE400 && addr <= 0xE5FF) {
        s->palram[addr - 0xE400] = val;
        // Recalcular color afectado (2 bytes por entrada)
        pbaction_build_palette(s);
        return;
    }

    // e600: nmi_mask
    if (addr == 0xE600) {
        s->nmi_mask = val & 1;
        return;
    }

    // e604: flipscreen
    if (addr == 0xE604) {
        s->flipscreen = val & 1;
        return;
    }

    // e606: scroll BG
    if (addr == 0xE606) {
        s->bg_scroll = val;
        return;
    }

    // e800: comando a CPU sonido
    if (addr == 0xE800) {
        s->sound_latch = val;
        // en MAME se dispara IRQ vector 0x00 en audiocpu al escribir soundlatch
        pb_z80_irq_vec(&s->audiocpu, 0x00);
        return;
    }
}

static uint8_t main_port_in(z80* z, uint16_t port) {
    (void)port;
    return 0xFF;
}

static void main_port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z; (void)port; (void)val;
}

// ----------------------------------------------------------------------------
// Callbacks Z80 - audio
// ----------------------------------------------------------------------------

static uint8_t audio_mem_read(void* user, uint16_t addr) {
    PBAction* s = (PBAction*)user;
    (void)s;

    if (addr < 0x2000) return pbaction_audio_rom[addr];
    if (addr >= 0x4000 && addr <= 0x47FF) return pbaction_audio_ram[addr - 0x4000];
    if (addr == 0x8000) return s->sound_latch;
    return 0xFF;
}

static void audio_mem_write(void* user, uint16_t addr, uint8_t val) {
    (void)user;
    if (addr >= 0x4000 && addr <= 0x47FF) {
        pbaction_audio_ram[addr - 0x4000] = val;
        return;
    }
    // 0xffff watchdog nop
    (void)addr; (void)val;
}

// AY stub: mapeo de puertos 0x10/0x11, 0x20/0x21, 0x30/0x31 (mask 0xff)
// Guardamos registro seleccionado y valores escritos por chip.

typedef struct {
    uint8_t reg;
    uint8_t regs[16];
} AYStub;

static AYStub ay[3];

static uint8_t audio_port_in(z80* z, uint16_t port) {
    
    port &= 0xFF;
    // no hay lecturas usadas aquí (podría haber)
    return 0xFF;
}

static void ay_addr_data_write(int idx, uint16_t port, uint8_t val) {
    // En MAME: ay8910_address_data_w se usa con dos puertos consecutivos
    //  - puerto par: address
    //  - puerto impar: data
    if ((port & 1) == 0) {
        ay[idx].reg = val & 0x0F;
    } else {
        ay[idx].regs[ay[idx].reg & 0x0F] = val;
    }
}

static void audio_port_out(z80* z, uint16_t port, uint8_t val) {
    
    port &= 0xFF;
    if (port == 0x10 || port == 0x11) { ay_addr_data_write(0, port, val); return; }
    if (port == 0x20 || port == 0x21) { ay_addr_data_write(1, port, val); return; }
    if (port == 0x30 || port == 0x31) { ay_addr_data_write(2, port, val); return; }
}

// ----------------------------------------------------------------------------
// Paleta (xxxxBBBBGGGGRRRR, little-endian)
// ----------------------------------------------------------------------------

static inline uint8_t pal4_to_8(uint8_t v) {
    // 0..15 -> 0..255
    return (uint8_t)((v << 4) | v);
}

void pbaction_build_palette(PBAction* s) {
    for (int i = 0; i < PBA_NUM_COLORS; i++) {
        int o = i * 2;
        uint16_t w = (uint16_t)s->palram[o] | ((uint16_t)s->palram[o + 1] << 8);
        uint8_t r = pal4_to_8((uint8_t)(w & 0x0F));
        uint8_t g = pal4_to_8((uint8_t)((w >> 4) & 0x0F));
        uint8_t b = pal4_to_8((uint8_t)((w >> 8) & 0x0F));
        s->palette[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ----------------------------------------------------------------------------
// Decodificación de GFX (planar por fracciones)
// ----------------------------------------------------------------------------

static inline uint8_t get_bit_msb(uint8_t byte, int x) {
    // x=0 => bit7
    return (byte >> (7 - x)) & 1;
}

// 8x8, 3bpp, region_size=0x6000 planes separados por 1/3
static uint8_t fg_pixel(PBAction* s, int code, int x, int y) {
    const int planes = 3;
    const int plane_sz = (int)(sizeof(s->fgchars) / planes);
    uint8_t p = 0;
    for (int pl = 0; pl < planes; pl++) {
        const uint8_t* base = &s->fgchars[pl * plane_sz];
        uint8_t rowb = base[(code & 0x7FF) * 8 + y];
        p |= (uint8_t)(get_bit_msb(rowb, x) << pl);
    }
    return p;
}

// 8x8, 4bpp, region_size=0x10000 planes por 1/4
static uint8_t bg_pixel(PBAction* s, int code, int x, int y) {
    const int planes = 4;
    const int plane_sz = (int)(sizeof(s->bgchars) / planes);
    uint8_t p = 0;
    for (int pl = 0; pl < planes; pl++) {
        const uint8_t* base = &s->bgchars[pl * plane_sz];
        uint8_t rowb = base[(code & 0xFFF) * 8 + y];
        p |= (uint8_t)(get_bit_msb(rowb, x) << pl);
    }
    return p;
}

// 16x16, 3bpp, region_size=0x6000, 1/3
static uint8_t spr16_pixel(PBAction* s, int code, int x, int y) {
    const int planes = 3;
    const int plane_sz = (int)(sizeof(s->sprites) / planes);
    uint8_t p = 0;
    // cada sprite 16x16: 32 bytes por plano (según layout 32*8)
    int base_off = (code & 0x7F) * 32;
    int row = y;
    int byte_off = base_off + (row * 2) + (x >> 3);
    for (int pl = 0; pl < planes; pl++) {
        const uint8_t* base = &s->sprites[pl * plane_sz];
        uint8_t b = base[byte_off];
        int xb = x & 7;
        p |= (uint8_t)(get_bit_msb(b, xb) << pl);
    }
    return p;
}

// 32x32 grande: usamos el layout MAME (spritelayout2) simplificado
// Para una implementación exacta, lo ideal es replicar los x/y offsets de MAME.
static uint8_t spr32_pixel(PBAction* s, int code, int x, int y) {
    // Descomponer en 4 subtiles 16x16 del mismo code base
    int subx = x >> 4;
    int suby = y >> 4;
    int sub = suby * 2 + subx; // 0..3
    int subcode = (code & 0x7F) * 4 + sub;
    return spr16_pixel(s, subcode, x & 15, y & 15);
}

// ----------------------------------------------------------------------------
// Render: tilemaps + sprites
// ----------------------------------------------------------------------------

static inline uint32_t pal_pen(PBAction* s, int pen) {
    return s->palette[pen & 0xFF];
}

static void clear_log(PBAction* s) {
    for (int i = 0; i < PBA_LOG_W * PBA_LOG_H; i++) s->logbuf[i] = 0xFF000000u;
}

static void draw_bg(PBAction* s) {
    // 32x32 tilemap, visible 256x224; aplicamos scroll vertical (en el eje X lógico) como MAME sugiere.
    // En el driver clásico se comenta "bg scroll" en e606.
    int scroll = s->bg_scroll;

    for (int ty = 0; ty < PBA_TILES_Y; ty++) {
        for (int tx = 0; tx < PBA_TILES_X; tx++) {
            int offs = ty * PBA_TILES_X + tx;
            int code = s->videoram_bg[offs];
            int attr = s->colorram_bg[offs];
            int color = attr & 0x07; // 8 grupos (según GFXDECODE_ENTRY bgchars..., 8)

            int sx = tx * 8;
            int sy = ty * 8;

            for (int py = 0; py < 8; py++) {
                int y = sy + py;
                if (y < 0 || y >= PBA_LOG_H) continue;
                for (int px = 0; px < 8; px++) {
                    int x = (sx + px + scroll) & 0xFF; // wrap 256
                    if (x < 0 || x >= PBA_LOG_W) continue;
                    uint8_t pix = bg_pixel(s, code, px, py);
                    int pen = 128 + color * 16 + pix; // base 128
                    s->logbuf[y * PBA_LOG_W + x] = pal_pen(s, pen);
                }
            }
        }
    }
}

static void draw_fg(PBAction* s) {
    for (int ty = 0; ty < PBA_TILES_Y; ty++) {
        for (int tx = 0; tx < PBA_TILES_X; tx++) {
            int offs = ty * PBA_TILES_X + tx;
            int code = s->videoram_fg[offs];
            int attr = s->colorram_fg[offs];
            int color = attr & 0x0F; // 16 grupos

            int sx = tx * 8;
            int sy = ty * 8;

            // recorte visible: 224 líneas
            if (sy >= PBA_LOG_H) continue;

            for (int py = 0; py < 8; py++) {
                int y = sy + py;
                if (y < 0 || y >= PBA_LOG_H) continue;
                for (int px = 0; px < 8; px++) {
                    int x = sx + px;
                    if (x < 0 || x >= PBA_LOG_W) continue;

                    uint8_t pix = fg_pixel(s, code, px, py);
                    if (pix == 0) continue; // transparencia pen0
                    int pen = color * 8 + pix;
                    s->logbuf[y * PBA_LOG_W + x] = pal_pen(s, pen);
                }
            }
        }
    }
}

static void draw_sprites(PBAction* s) {
    // Formato aproximado: 32 sprites x 4 bytes.
    // offs: 0..0x7C step4
    for (int offs = 0; offs < PBA_SPRRAM_SIZE; offs += 4) {
        uint8_t sy = s->spriteram[offs + 0];
        uint8_t code = s->spriteram[offs + 1];
        uint8_t attr = s->spriteram[offs + 2];
        uint8_t sx = s->spriteram[offs + 3];

        int color = attr & 0x0F;
        bool flipx = (attr & 0x10) != 0;
        bool flipy = (attr & 0x20) != 0;
        bool big   = (attr & 0x80) != 0;

        int w = big ? 32 : 16;
        int h = big ? 32 : 16;

        // coordenadas típicas en tilesets: y invertida
        int x0 = (int)sx;
        int y0 = (int)(240 - sy); // heurística común

        for (int y = 0; y < h; y++) {
            int yy = y0 + y;
            if (yy < 0 || yy >= PBA_LOG_H) continue;
            for (int x = 0; x < w; x++) {
                int xx = x0 + x;
                if (xx < 0 || xx >= PBA_LOG_W) continue;

                int px = flipx ? (w - 1 - x) : x;
                int py = flipy ? (h - 1 - y) : y;

                uint8_t pix = big ? spr32_pixel(s, code, px, py) : spr16_pixel(s, code, px, py);
                if (pix == 0) continue;
                int pen = color * 8 + pix;
                s->logbuf[yy * PBA_LOG_W + xx] = pal_pen(s, pen);
            }
        }
    }
}

// ROT90 CW: dst(x,y) = src(x_src, y_src)
// Si src es 256x224, dst es 224x256:
//   dst_x = (PBA_LOG_H - 1 - src_y)
//   dst_y = src_x
// Inverso (para generar dst):
//   src_x = dst_y
//   src_y = (PBA_LOG_H - 1 - dst_x)
static void blit_rot90_cw(PBAction* s) {
    for (int dy = 0; dy < PBA_SCREEN_H; dy++) {
        for (int dx = 0; dx < PBA_SCREEN_W; dx++) {
            int sx = dy;
            int sy = (PBA_LOG_H - 1 - dx);
            uint32_t p = s->logbuf[sy * PBA_LOG_W + sx];

            // flipscreen (aprox): invertir en ambos ejes ya rotados
            if (s->flipscreen) {
                int fx = (PBA_SCREEN_W - 1) - dx;
                int fy = (PBA_SCREEN_H - 1) - dy;
                s->framebuffer[fy * PBA_SCREEN_W + fx] = p;
            } else {
                s->framebuffer[dy * PBA_SCREEN_W + dx] = p;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Init/SDL
// ----------------------------------------------------------------------------

void pbaction_init(PBAction* s) {
    memset(s, 0, sizeof(*s));

    // Inputs default (active-high): nada pulsado
    s->in_p1  = 0x00;
    s->in_p2  = 0x00;
    s->in_sys = 0x00;

    // DSW por defecto (aprox): todo a 0
    s->dsw1 = 0x00;
    s->dsw2 = 0x00;

    // SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    s->window = SDL_CreateWindow("Pinball Action (Tehkan, 1985)",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 PBA_SCREEN_W * PBA_SCALE,
                                 PBA_SCREEN_H * PBA_SCALE,
                                 SDL_WINDOW_SHOWN);
    s->renderer = SDL_CreateRenderer(s->window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(s->renderer, PBA_SCREEN_W, PBA_SCREEN_H);
    s->texture  = SDL_CreateTexture(s->renderer,
                                    SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    PBA_SCREEN_W, PBA_SCREEN_H);

    // CPUs
    z80_init(&s->maincpu);
    s->maincpu.userdata   = s;
    s->maincpu.read_byte  = main_mem_read;
    s->maincpu.write_byte = main_mem_write;
    s->maincpu.port_in    = main_port_in;
    s->maincpu.port_out   = main_port_out;

    z80_init(&s->audiocpu);
    s->audiocpu.userdata   = s;
    s->audiocpu.read_byte  = audio_mem_read;
    s->audiocpu.write_byte = audio_mem_write;
    s->audiocpu.port_in    = audio_port_in;
    s->audiocpu.port_out   = audio_port_out;

    pb_z80_reset(&s->maincpu);
    pb_z80_reset(&s->audiocpu);

    pbaction_build_palette(s);
}

void pbaction_shutdown(PBAction* s) {
    if (s->texture)  SDL_DestroyTexture(s->texture);
    if (s->renderer) SDL_DestroyRenderer(s->renderer);
    if (s->window)   SDL_DestroyWindow(s->window);
    SDL_Quit();
    (void)s;
}

// ----------------------------------------------------------------------------
// Run frame
// ----------------------------------------------------------------------------

static inline int cycles_per_frame(int hz) {
    return hz / PBA_FPS;
}

void pbaction_run_frame(PBAction* s) {
    int cyc_main  = cycles_per_frame(PBA_MAIN_HZ);
    int cyc_audio = cycles_per_frame(PBA_AUDIO_HZ);

    // Ejecutar de forma intercalada para aproximar sincronía
    while (cyc_main > 0 || cyc_audio > 0) {
        if (cyc_main > 0) {
            int ran = z80_step(&s->maincpu);
            if (ran <= 0) ran = 1;
            cyc_main -= ran;
        }
        if (cyc_audio > 0) {
            int ran = z80_step(&s->audiocpu);
            if (ran <= 0) ran = 1;
            cyc_audio -= ran;
        }
    }

    // VBLANK: el driver usa NMI en vblank gated por nmi_mask.
    if (s->nmi_mask) {
        pb_z80_pulse_nmi(&s->maincpu);
    }
}

// ----------------------------------------------------------------------------
// Render
// ----------------------------------------------------------------------------

void pbaction_render(PBAction* s) {
    clear_log(s);
    draw_bg(s);
    draw_sprites(s);
    draw_fg(s);
    blit_rot90_cw(s);

    SDL_UpdateTexture(s->texture, NULL, s->framebuffer, PBA_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(s->renderer);
    SDL_RenderCopy(s->renderer, s->texture, NULL, NULL);
    SDL_RenderPresent(s->renderer);
}

// ----------------------------------------------------------------------------
// Input mapping (teclas)
// ----------------------------------------------------------------------------

static inline void set_bit(uint8_t* v, uint8_t mask, bool pressed) {
    if (pressed) *v |= mask; else *v &= (uint8_t)~mask;
}

void pbaction_handle_key(PBAction* s, SDL_Scancode sc, bool pressed) {
    // P1/P2: según MAME: botones 1..4 en bits 0x08,0x10,0x01,0x04 (P1)
    switch (sc) {
        case SDL_SCANCODE_Z:   set_bit(&s->in_p1, 0x08, pressed); break; // button1
        case SDL_SCANCODE_X:   set_bit(&s->in_p1, 0x10, pressed); break; // button2
        case SDL_SCANCODE_C:   set_bit(&s->in_p1, 0x01, pressed); break; // button3
        case SDL_SCANCODE_V:   set_bit(&s->in_p1, 0x04, pressed); break; // button4

        case SDL_SCANCODE_1:   set_bit(&s->in_sys, 0x04, pressed); break; // start1
        case SDL_SCANCODE_2:   set_bit(&s->in_sys, 0x08, pressed); break; // start2
        case SDL_SCANCODE_5:   set_bit(&s->in_sys, 0x01, pressed); break; // coin1
        case SDL_SCANCODE_6:   set_bit(&s->in_sys, 0x02, pressed); break; // coin2

        case SDL_SCANCODE_TAB:
            if (pressed) s->turbo = !s->turbo;
            break;

        case SDL_SCANCODE_ESCAPE:
            if (pressed) s->quit = true;
            break;

        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// main / CLI
// ----------------------------------------------------------------------------

static void usage(const char* exe) {
    printf("Uso: %s --dir RUTA\n\n", exe);
    printf("Directorios de ROMs (set pbaction):\n");
    printf("  maincpu : b-p7.bin b-n7.bin b-l7.bin\n");
    printf("  audiocpu: a-e3.bin\n");
    printf("  fgchars : a-s6.bin a-s7.bin a-s8.bin\n");
    printf("  bgchars : a-j5.bin a-j6.bin a-j7.bin a-j8.bin\n");
    printf("  sprites : b-c7.bin b-d7.bin b-f7.bin\n\n");
    printf("Controles:\n");
    printf("  Z/X/C/V = botones 1/2/3/4\n");
    printf("  5/6     = coin1/coin2\n");
    printf("  1/2     = start1/start2\n");
    printf("  TAB     = turbo\n");
    printf("  ESC     = salir\n");
}

int main(int argc, char** argv) {
    const char* dir = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--dir") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Opción desconocida: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!dir) {
        usage(argv[0]);
        return 1;
    }

    PBAction st;
    pbaction_init(&st);

    if (pbaction_load_from_dir(&st, dir) != 0) {
        fprintf(stderr, "Error cargando ROMs desde %s\n", dir);
        pbaction_shutdown(&st);
        return 1;
    }

    pbaction_build_palette(&st);

    const uint32_t frame_ms = 1000 / PBA_FPS;

    while (!st.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) st.quit = true;
            else if (e.type == SDL_KEYDOWN) pbaction_handle_key(&st, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP) pbaction_handle_key(&st, e.key.keysym.scancode, false);
        }

        pbaction_run_frame(&st);

        if (!st.turbo) {
            pbaction_render(&st);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
        } else {
            // en turbo, render menos a menudo
            static int ctr = 0;
            if ((ctr++ & 7) == 0) pbaction_render(&st);
        }
    }

    pbaction_shutdown(&st);
    return 0;
}
