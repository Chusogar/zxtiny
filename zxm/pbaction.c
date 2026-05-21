/*
 * pbaction.c  -  Emulador (mini) de Pinball Action (Tehkan, 1985)
 *
 * Objetivo: replicar la misma filosof a que phoenix.c:
 *   - todo en 1 .c + 1 .h
 *   - cargar ROMs desde una carpeta (--dir)
 *   - emular lo esencial de CPU+memmap+video+inputs
 *   - sonido: AY-3-8910 x3 por SDL (S16 mono) + debug (F3 tono, F4 ganancia, logs)
 *
 * Referencias (hardware / mapas / GFX):
 *   - Driver cl sico MAME (pbaction.c) con mapa de memoria, entradas y layouts.
 *
 * Compilar (ejemplo):
 *   gcc pbaction.c z80/jgz80/z80.c -o pbaction -lSDL2 -lm -O2
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
#include <math.h>

// ----------------------------------------------------------------------------
// (Opcional) API de interrupciones del core Z80
// ----------------------------------------------------------------------------
// Nota: phoenix no usa IRQ/NMI. Pinball Action s .
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
//#ifdef JGZ80_HAVE_IRQ
    z80_pulse_irq(cpu, vec);
//#else
//    (void)cpu; (void)vec;
//#endif
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

    // b squeda case-insensitive por nombre sin extension
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
    // reutilizamos s->ram temporalmente NO; creamos un buffer local est tico?
    // Para simplicidad, a adimos un buffer de audio ROM embebido en PBAction via malloc.
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
    const char* sp_names[3] = {"b-c7.bin", "b-d7.bin", "b-f7.bin"};
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

// ----------------------------------------------------------------------------
// AY-3-8910 (x3) + salida de audio por SDL
// ----------------------------------------------------------------------------
// Pinball Action usa 3 chips AY-3-8910 controlados por la CPU de sonido.
// El clock exacto depende de la placa; aproximamos AY_CLOCK = PBA_AUDIO_HZ/2.
// El AY divide internamente por 16 para su base de tiempo.

#define PBA_AY_CHIPS 3
#define PBA_AY_CLOCK (PBA_AUDIO_HZ/2)

typedef struct {
    uint8_t regs[16];
    uint8_t addr;

    // 16.16 fixed-point, unidades en (AY_CLOCK/16)
    uint32_t tone_period_fp[3];
    uint32_t tone_count_fp[3];
    uint8_t  tone_out[3];

    uint32_t noise_period_fp;
    uint32_t noise_count_fp;
    uint32_t lfsr; // 17-bit
    uint8_t  noise_out;

    uint32_t env_period_fp;
    uint32_t env_count_fp;
    uint8_t  env_shape;
    uint8_t  env_hold_active;
    int      env_dir;
    uint8_t  env_phase;
    uint8_t  env_vol;
} AY8910;

static AY8910 g_ay[PBA_AY_CHIPS];
static uint32_t g_ay_ticks_per_sample_fp = 0; // 16.16

// HP (quita DC)
static float g_hp_prev_x = 0.0f;
static float g_hp_prev_y = 0.0f;

// Debug: detectar si se ha escrito algún volumen no-cero
static uint32_t g_ay_data_writes = 0;
static int g_any_nonzero_vol = 0;

// Tabla de volumen (aprox)
static const float g_ay_voltbl[16] = {
    0.0000f, 0.0040f, 0.0060f, 0.0090f,
    0.0130f, 0.0190f, 0.0280f, 0.0420f,
    0.0630f, 0.0940f, 0.1410f, 0.2110f,
    0.3160f, 0.4730f, 0.7070f, 1.0000f
};

static inline uint32_t ay_make_fp(uint32_t v) { return v << 16; }

static void ay_recalc_periods(AY8910* a) {
    // Tono: 12-bit regs 0..5
    for (int ch = 0; ch < 3; ch++) {
        int r = ch * 2;
        uint32_t p = (uint32_t)a->regs[r] | (((uint32_t)a->regs[r + 1] & 0x0F) << 8);
        if (p == 0) p = 1;
        a->tone_period_fp[ch] = ay_make_fp(p);
    }

    // Ruido: 5-bit reg 6
    uint32_t np = (uint32_t)(a->regs[6] & 0x1F);
    if (np == 0) np = 1;
    a->noise_period_fp = ay_make_fp(np);

    // Envolvente: 16-bit regs 11/12
    uint32_t ep = (uint32_t)a->regs[11] | ((uint32_t)a->regs[12] << 8);
    if (ep == 0) ep = 1;
    a->env_period_fp = ay_make_fp(ep);
}

static void ay_env_restart(AY8910* a, uint8_t shape) {
    a->env_shape = shape & 0x0F;
    a->env_hold_active = 0;
    a->env_phase = 0;

    // C=bit3, A=bit2, ALT=bit1, HOLD=bit0
    int attack = (a->env_shape & 0x04) ? 1 : 0;
    a->env_dir = attack ? +1 : -1;
    a->env_vol = attack ? 0 : 15;
    a->env_count_fp = 0;
}

static inline uint8_t ay_env_continue(const AY8910* a) { return (a->env_shape & 0x08) ? 1 : 0; }
static inline uint8_t ay_env_attack  (const AY8910* a) { return (a->env_shape & 0x04) ? 1 : 0; }
static inline uint8_t ay_env_alt     (const AY8910* a) { return (a->env_shape & 0x02) ? 1 : 0; }
static inline uint8_t ay_env_hold    (const AY8910* a) { return (a->env_shape & 0x01) ? 1 : 0; }

static void ay_step_envelope(AY8910* a) {
    if (a->env_hold_active) return;

    a->env_phase++;
    if (a->env_phase >= 16) {
        if (!ay_env_continue(a)) {
            a->env_hold_active = 1;
            a->env_vol = 0;
            return;
        }
        if (ay_env_hold(a)) {
            a->env_hold_active = 1;
            a->env_vol = ay_env_attack(a) ? 15 : 0;
            return;
        }
        if (ay_env_alt(a)) a->env_dir = -a->env_dir;
        a->env_phase = 0;
    }

    a->env_vol = (a->env_dir > 0) ? a->env_phase : (uint8_t)(15 - a->env_phase);
}

static void ay_reset(AY8910* a) {
    memset(a, 0, sizeof(*a));
    a->lfsr = 0x1FFFFu;
    a->noise_out = 1;
    for (int ch = 0; ch < 3; ch++) {
        a->tone_out[ch] = 0;
        a->tone_count_fp[ch] = 0;
    }
    ay_recalc_periods(a);
    ay_env_restart(a, 0);
}

static void ay_write_reg(AY8910* a, int r, uint8_t v) {
    r &= 0x0F;
    a->regs[r] = v;
    if (r <= 6 || r == 11 || r == 12) ay_recalc_periods(a);
    if (r == 13) ay_env_restart(a, v);
}

static uint8_t ay_read_reg(const AY8910* a) {
    return a->regs[a->addr & 0x0F];
}

static inline int16_t clamp16(int v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

static float ay_render_sample(AY8910* a, uint32_t ticks_fp) {
    // Tono
    for (int ch = 0; ch < 3; ch++) {
        a->tone_count_fp[ch] += ticks_fp;
        uint32_t per = a->tone_period_fp[ch];
        while (a->tone_count_fp[ch] >= per) {
            a->tone_count_fp[ch] -= per;
            a->tone_out[ch] ^= 1;
        }
    }

    // Ruido (LFSR 17-bit, newbit = bit0 XOR bit3)
    a->noise_count_fp += ticks_fp;
    while (a->noise_count_fp >= a->noise_period_fp) {
        a->noise_count_fp -= a->noise_period_fp;
        uint32_t bit0 = a->lfsr & 1u;
        uint32_t bit3 = (a->lfsr >> 3) & 1u;
        uint32_t newb = bit0 ^ bit3;
        a->lfsr = (a->lfsr >> 1) | (newb << 16);
        a->noise_out = (uint8_t)(a->lfsr & 1u);
    }

    // Envolvente
    a->env_count_fp += ticks_fp;
    while (a->env_count_fp >= a->env_period_fp) {
        a->env_count_fp -= a->env_period_fp;
        ay_step_envelope(a);
    }

    // Mezcla
    uint8_t enable = a->regs[7];
    float out = 0.0f;

    for (int ch = 0; ch < 3; ch++) {
        int tone_dis  = (enable >> ch) & 1;
        int noise_dis = (enable >> (ch + 3)) & 1;

        float tone_sig = tone_dis ? 1.0f : (a->tone_out[ch] ? 1.0f : -1.0f);
        float noise_gate = noise_dis ? 1.0f : (a->noise_out ? 1.0f : 0.0f);

        uint8_t vr = a->regs[8 + ch];
        uint8_t v = (vr & 0x10) ? a->env_vol : (vr & 0x0F);
        float amp = g_ay_voltbl[v & 0x0F];

        out += tone_sig * noise_gate * amp;
    }

    return out;
}

static inline void pb_audio_lock(PBAction* s) {
    if (s && s->audio_dev) SDL_LockAudioDevice(s->audio_dev);
}
static inline void pb_audio_unlock(PBAction* s) {
    if (s && s->audio_dev) SDL_UnlockAudioDevice(s->audio_dev);
}

static void pb_audio_callback(void* userdata, Uint8* stream, int len) {
    PBAction* s = (PBAction*)userdata;
    if (!s || !s->audio_enabled || g_ay_ticks_per_sample_fp == 0) {
        memset(stream, 0, (size_t)len);
        return;
    }

    int16_t* out = (int16_t*)stream;
    int nsamp = len / (int)sizeof(int16_t);

    static float phase = 0.0f;
    const float hp_a = 0.995f;

    for (int i = 0; i < nsamp; i++) {
        float mix;

        // Debug útil: si no hay volúmenes no-cero, mantenemos tono de test
        if (s->audio_test_tone || (!g_any_nonzero_vol)) {
            float inc = 2.0f * 3.14159265f * 440.0f / (float)(s->audio_rate ? s->audio_rate : 44100);
            phase += inc;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
            mix = 0.20f * (float)sin(phase);
        } else {
            mix = 0.0f;
            for (int k = 0; k < PBA_AY_CHIPS; k++) {
                mix += ay_render_sample(&g_ay[k], g_ay_ticks_per_sample_fp);
            }
            mix *= (1.0f / (float)PBA_AY_CHIPS);
            mix *= (s->audio_gain > 0.0f ? s->audio_gain : 0.25f);
        }

        // HP: y[n] = x[n] - x[n-1] + a*y[n-1]
        float y = mix - g_hp_prev_x + hp_a * g_hp_prev_y;
        g_hp_prev_x = mix;
        g_hp_prev_y = y;

        out[i] = clamp16((int)(y * 32767.0f));
    }
}

static uint8_t audio_port_in(z80* z, uint16_t port) {
    PBAction* s = (PBAction*)z->userdata;
    port &= 0xFF;

    pb_audio_lock(s);
    uint8_t v = 0xFF;
    if (port == 0x11) v = ay_read_reg(&g_ay[0]);
    else if (port == 0x21) v = ay_read_reg(&g_ay[1]);
    else if (port == 0x31) v = ay_read_reg(&g_ay[2]);
    pb_audio_unlock(s);

    return v;
}

static void audio_port_out(z80* z, uint16_t port, uint8_t val) {
    PBAction* s = (PBAction*)z->userdata;
    port &= 0xFF;

    pb_audio_lock(s);

    // Log 1er acceso a puertos AY
    if (s) {
        s->ay_write_count++;
        if (s->ay_write_count == 1) {
            fprintf(stderr, "[AUDIO] primer write a puertos AY (port=0x%02X val=0x%02X)\n", (unsigned)port, (unsigned)val);
        }
    }

    // Cada AY usa dos puertos consecutivos: par=address / impar=data
    if (port == 0x10) { g_ay[0].addr = val & 0x0F; pb_audio_unlock(s); return; }
    if (port == 0x20) { g_ay[1].addr = val & 0x0F; pb_audio_unlock(s); return; }
    if (port == 0x30) { g_ay[2].addr = val & 0x0F; pb_audio_unlock(s); return; }

    if (port == 0x11) {
        // Debug: log primeras escrituras DATA
        if (g_ay_data_writes < 32) {
            fprintf(stderr, "[AUDIO] AY0 reg%u = 0x%02X (port 0x11)\n", (unsigned)(g_ay[0].addr & 0x0F), (unsigned)val);
        }
        g_ay_data_writes++;
        uint8_t r = g_ay[0].addr & 0x0F;
        if ((r >= 8 && r <= 10) && (val & 0x1F)) g_any_nonzero_vol = 1;
        ay_write_reg(&g_ay[0], g_ay[0].addr, val);
        pb_audio_unlock(s);
        return;
    }

    if (port == 0x21) {
        if (g_ay_data_writes < 32) {
            fprintf(stderr, "[AUDIO] AY1 reg%u = 0x%02X (port 0x21)\n", (unsigned)(g_ay[1].addr & 0x0F), (unsigned)val);
        }
        g_ay_data_writes++;
        uint8_t r = g_ay[1].addr & 0x0F;
        if ((r >= 8 && r <= 10) && (val & 0x1F)) g_any_nonzero_vol = 1;
        ay_write_reg(&g_ay[1], g_ay[1].addr, val);
        pb_audio_unlock(s);
        return;
    }

    if (port == 0x31) {
        if (g_ay_data_writes < 32) {
            fprintf(stderr, "[AUDIO] AY2 reg%u = 0x%02X (port 0x31)\n", (unsigned)(g_ay[2].addr & 0x0F), (unsigned)val);
        }
        g_ay_data_writes++;
        uint8_t r = g_ay[2].addr & 0x0F;
        if ((r >= 8 && r <= 10) && (val & 0x1F)) g_any_nonzero_vol = 1;
        ay_write_reg(&g_ay[2], g_ay[2].addr, val);
        pb_audio_unlock(s);
        return;
    }

    pb_audio_unlock(s);
}

// ----------------------------------------------------------------------------
// Paleta (xxxxBBBBGGGGRRRR, little-endian) + update por entrada
// ----------------------------------------------------------------------------
static inline uint8_t pal4_to_8(uint8_t v) {
    return (uint8_t)((v << 4) | v);
}

static void pbaction_update_palette_entry(PBAction* s, int i) {
    int o = i * 2;
    uint16_t w = (uint16_t)s->palram[o + 0] | ((uint16_t)s->palram[o + 1] << 8);

    uint8_t r4 = (uint8_t)((w >> 0) & 0x0F);
    uint8_t g4 = (uint8_t)((w >> 4) & 0x0F);
    uint8_t b4 = (uint8_t)((w >> 8) & 0x0F);

    uint8_t r = pal4_to_8(r4);
    uint8_t g = pal4_to_8(g4);
    uint8_t b = pal4_to_8(b4);

    s->palette[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void pbaction_build_palette(PBAction* s) {
    for (int i = 0; i < PBA_NUM_COLORS; i++) pbaction_update_palette_entry(s, i);
}

// ----------------------------------------------------------------------------
// Decodificaci n de GFX (planar por fracciones)
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
    // cada sprite 16x16: 32 bytes por plano (seg n layout 32*8)
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
// Para una implementaci n exacta, lo ideal es replicar los x/y offsets de MAME.
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

static void draw_bg(PBAction* s)
{
    // BG: tilemap 32x32 de 8x8 => 256x256.
    // Visible en el "logical buffer": 256x224 (PBA_LOG_W x PBA_LOG_H). [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.h)
    //
    // s->bg_scroll: scroll (8-bit). En muchos boards Tehkan el BG scroll es vertical
    // respecto al  rea visible. Como aqu  trabajamos en buffer NO rotado (256x224),
    // aplicamos el scroll sobre Y del mundo (map 256x256). [1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.c)
    //
    // Atributos t picos:
    //  - code: videoram_bg[offs] + bits extra desde colorram_bg
    //  - color: nibble bajo de colorram_bg
    //
    // Nota: Si el bit 4-7 de colorram se usan como code-high, lo soportamos.

    const int scroll = (int)s->bg_scroll;  // 0..255 [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.h)

    for (int sy = 0; sy < PBA_LOG_H; sy++) {
        // y dentro del tilemap 256x256 con scroll
        int my = (sy + scroll) & 0xFF;        // 0..255
        int ty = (my >> 3) & 31;              // tile y 0..31
        int py = my & 7;                      // pixel en tile 0..7

        for (int sx = 0; sx < PBA_LOG_W; sx++) {
            int mx = sx & 0xFF;               // 0..255
            int tx = (mx >> 3) & 31;          // tile x 0..31
            int px = mx & 7;                  // pixel en tile 0..7

            // Flipscreen: invierte coordenadas dentro del buffer l gico
            int dsx = sx;
            int dsy = sy;
            int fpx = px;
            int fpy = py;
            int ftx = tx;
            int fty = ty;

            if (s->flipscreen) {
                // flip total del  rea visible 256x224
                dsx = (PBA_LOG_W - 1) - sx;
                dsy = (PBA_LOG_H - 1) - sy;

                // flip dentro del tilemap 256x256
                mx = (255 - sx) & 0xFF;
                my = (255 - ((sy + scroll) & 0xFF)) & 0xFF;

                ftx = (mx >> 3) & 31;
                fty = (my >> 3) & 31;
                fpx = mx & 7;
                fpy = my & 7;
            }

            int offs = fty * PBA_TILES_X + ftx;                  // 32x32 [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.h)
            uint8_t v  = s->videoram_bg[offs];
            uint8_t a  = s->colorram_bg[offs];

            // code: base + bits altos (muy t pico en esta familia)
            int code  = (int)v | ((int)(a & 0xF0) << 4);          // 0..4095 aprox
            int color = (int)(a & 0x0F);                          // 0..15

            // Pixel 4bpp
            uint8_t pen = bg_pixel(s, code, fpx, fpy);            // 0..15 [1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.c)

            // BG normalmente NO es transparente. Si en tu PCB pen=0 debe ser transparente,
            // descomenta el if:
            // if (pen == 0) continue;

            // Paleta BG: 4bpp (16 pens) por color (16 grupos) => 256 entradas.
            int pal_index = (color << 4) | (pen & 0x0F);

            s->logbuf[dsy * PBA_LOG_W + dsx] = pal_pen(s, pal_index);
        }
    }
}

static void draw_fg(PBAction* s)
{
    // FG: tilemap 32x32 de 8x8 => 256x256.
    // Visible en el buffer l gico: 256x224 (PBA_LOG_W x PBA_LOG_H). [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.h)
    // fg_pixel(): 8x8, 3bpp, pen 0..7 (pen 0 transparente). [1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.c)

    for (int ty = 0; ty < PBA_TILES_Y; ty++) {
        for (int tx = 0; tx < PBA_TILES_X; tx++) {
            int offs = ty * PBA_TILES_X + tx;

            uint8_t v = s->videoram_fg[offs];
            uint8_t a = s->colorram_fg[offs];

            // C digo: byte base + bits altos desde el nibble alto (mapeo t pico).
            // fg_pixel() ya enmascara a 0x7FF, as  que es seguro. [1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.c)
            int code  = (int)v | ((int)(a & 0xF0) << 4);

            // Color: low nibble (16 grupos)
            int color = (int)(a & 0x0F);

            // Dibuja tile 8x8
            for (int y = 0; y < PBA_TILE_H; y++) {
                int sy = ty * PBA_TILE_H + y;
                if (sy < 0 || sy >= PBA_LOG_H) continue; // recorte a 224 l neas visibles [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.h)

                for (int x = 0; x < PBA_TILE_W; x++) {
                    int sx = tx * PBA_TILE_W + x;
                    if (sx < 0 || sx >= PBA_LOG_W) continue;

                    int px = x;
                    int py = y;
                    int dsx = sx;
                    int dsy = sy;

                    // Flipscreen: invierte coordenadas dentro del  rea visible (256x224)
                    if (s->flipscreen) {
                        px  = (PBA_TILE_W - 1) - x;
                        py  = (PBA_TILE_H - 1) - y;
                        dsx = (PBA_LOG_W - 1) - sx;
                        dsy = (PBA_LOG_H - 1) - sy;
                    }

                    uint8_t pen = fg_pixel(s, code, px, py); // 0..7 [1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.c)
                    if (pen == 0) continue; // transparencia FG

                    //  ndice de paleta:
                    // Usamos la misma  banco de 16  que BG: (color<<4)|pen.
                    // Como pen es 0..7, encaja dentro del bloque del color. [2](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.h)[1](https://indra365-my.sharepoint.com/personal/jagsanchez_indra_es/Documents/Archivos%20de%20Microsoft%C2%A0Copilot%20Chat/pbaction.c)
                    int pal_index = (color << 4) | (pen & 0x0F);

                    s->logbuf[dsy * PBA_LOG_W + dsx] = pal_pen(s, pal_index);
                }
            }
        }
    }
}

// 32x32, 3bpp, layout2 real (MAME spritelayout2) usando REGION_GFX3 offset 0x01000
// Cada sprite 32x32 ocupa 128 bytes por plano. Hay 32 sprites (0..31). [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)
static uint8_t spr32_layout2_pixel(PBAction* s, int code, int x, int y)
{
    const int planes = 3;
    const int plane_sz = (int)(sizeof(s->sprites) / planes);

    // En MAME gfxdecode: REGION_GFX3, offset 0x01000 para sprites grandes [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)
    const int region_off = 0x01000;

    // 32 sprites grandes
    code &= 0x1F;

    // Cada sprite 32x32: 128 bytes por plano (charincrement = 128*8 bits) [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)
    int sprite_base = code * 128;

    // Offsets de byte por grupos según xoffset/yoffset del layout2 [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)
    static const int xbyte_off[4] = { 0, 8, 32, 40 };     // 0, 8*8, 32*8, 40*8 (bits) => 0,8,32,40 bytes
    int xb = x >> 3;                                     // 0..3
    int bit = x & 7;                                     // 0..7 (MSB primero)
    int xoff = xbyte_off[xb];

    int yoff;
    if (y < 8)       yoff = y;           // 0..7
    else if (y < 16) yoff = 16 + (y-8);  // 16..23
    else if (y < 24) yoff = 64 + (y-16); // 64..71
    else             yoff = 80 + (y-24); // 80..87

    int byte_off = sprite_base + yoff + xoff;

    uint8_t pen = 0;
    for (int pl = 0; pl < planes; pl++) {
        const uint8_t* base = &s->sprites[pl * plane_sz + region_off];
        uint8_t b = base[byte_off];
        pen |= (uint8_t)(((b >> (7 - bit)) & 1) << pl);
    }
    return pen;  // 0..7
}

static void draw_sprites(PBAction* s)
{
    // Formato real (MAME4ALL):
    //  offs+0: CODE (bit7 = 32x32)
    //  offs+1: ATTR (color low nibble, flipX=0x40, flipY=0x80)
    //  offs+2: Y
    //  offs+3: X [2](https://github.com/ValveSoftware/steamlink-sdk/blob/master/examples/mame4all/src/vidhrdw/pbaction.cpp)
    //
    // Orden: de final a principio para prioridad. [2](https://github.com/ValveSoftware/steamlink-sdk/blob/master/examples/mame4all/src/vidhrdw/pbaction.cpp)
    for (int offs = PBA_SPRRAM_SIZE - 4; offs >= 0; offs -= 4) {

        uint8_t codeb = s->spriteram[offs + 0];
        uint8_t attr  = s->spriteram[offs + 1];
        uint8_t yb    = s->spriteram[offs + 2];
        uint8_t xb    = s->spriteram[offs + 3];

        int big = (codeb & 0x80) ? 1 : 0;

        // Si el sprite "siguiente" es doble tamaño, este se ignora. [2](https://github.com/ValveSoftware/steamlink-sdk/blob/master/examples/mame4all/src/vidhrdw/pbaction.cpp)
        if (offs > 0 && (s->spriteram[offs - 4] & 0x80)) continue;

        int color = (attr & 0x0F);
        int flipx = (attr & 0x40) ? 1 : 0;
        int flipy = (attr & 0x80) ? 1 : 0;

        int sx = (int)xb;

        // Fórmulas Y distintas para 16x16 y 32x32. [2](https://github.com/ValveSoftware/steamlink-sdk/blob/master/examples/mame4all/src/vidhrdw/pbaction.cpp)
        int sy = big ? (225 - (int)yb) : (241 - (int)yb);

        // Flipscreen según MAME4ALL (ajustes distintos en big/normal). [2](https://github.com/ValveSoftware/steamlink-sdk/blob/master/examples/mame4all/src/vidhrdw/pbaction.cpp)
        if (s->flipscreen) {
            if (big) {
                sx = 224 - sx;
                sy = 225 - sy;
            } else {
                sx = 240 - sx;
                sy = 241 - sy;
            }
            flipx = !flipx;
            flipy = !flipy;
        }

        int w = big ? 32 : 16;
        int h = big ? 32 : 16;

        // Render por píxel con clipping al buffer lógico (256x224)
        for (int y = 0; y < h; y++) {
            int yy = flipy ? (h - 1 - y) : y;
            int py = sy + y;

            if ((unsigned)py >= (unsigned)PBA_LOG_H) continue;

            for (int x = 0; x < w; x++) {
                int xx = flipx ? (w - 1 - x) : x;
                int px = sx + x;

                if ((unsigned)px >= (unsigned)PBA_LOG_W) continue;

                uint8_t pen;
                if (!big) {
                    // sprites 16x16: 128 sprites, 3bpp [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)
                    pen = spr16_pixel(s, (int)(codeb & 0x7F), xx, yy);
                } else {
                    // sprites 32x32: layout2 en offset 0x01000, 3bpp [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)
                    pen = spr32_layout2_pixel(s, (int)codeb, xx, yy);
                }

                // transparencia: pen 0
                if (pen == 0) continue;

                // Paleta sprites: 16 colores * 8 pens = 128 entradas, base 0 [3](https://github.com/jv4779/openlase-mame/blob/master/xmame-0.106/src/drivers/pbaction.c)[2](https://github.com/ValveSoftware/steamlink-sdk/blob/master/examples/mame4all/src/vidhrdw/pbaction.cpp)
                int pal_index = (color << 3) | (pen & 0x07);

                s->logbuf[py * PBA_LOG_W + px] = pal_pen(s, pal_index);
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


    // Audio (SDL)
    s->audio_dev = 0;
    s->audio_rate = 0;
    s->audio_gain = 0.25f;
    s->audio_enabled = false;
    s->audio_test_tone = false;
    s->ay_write_count = 0;

    SDL_AudioSpec want;
    SDL_AudioSpec have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = pb_audio_callback;
    want.userdata = s;

    s->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s->audio_dev != 0) {
        s->audio_rate = have.freq;
        s->audio_enabled = true;

        uint32_t base = (uint32_t)(PBA_AY_CLOCK / 16);
        if (base == 0) base = 1;
        g_ay_ticks_per_sample_fp = (uint32_t)(((uint64_t)base << 16) / (uint32_t)(s->audio_rate ? s->audio_rate : 44100));

        for (int k = 0; k < PBA_AY_CHIPS; k++) ay_reset(&g_ay[k]);
        g_hp_prev_x = g_hp_prev_y = 0.0f;
        g_ay_data_writes = 0;
        g_any_nonzero_vol = 0;

        SDL_PauseAudioDevice(s->audio_dev, 0);
        fprintf(stderr, "[AUDIO] SDL freq=%dHz, AY base=%u -> ticks_fp=%u\n", s->audio_rate, base, g_ay_ticks_per_sample_fp);
    } else {
        fprintf(stderr, "[AUDIO] No se pudo abrir dispositivo SDL: %s\n", SDL_GetError());
    }

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
    if (s->audio_dev) { SDL_CloseAudioDevice(s->audio_dev); s->audio_dev = 0; }
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

    // Ejecutar de forma intercalada para aproximar sincron a
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
    // P1/P2: seg n MAME: botones 1..4 en bits 0x08,0x10,0x01,0x04 (P1)
    switch (sc) {
        case SDL_SCANCODE_Z:   set_bit(&s->in_p1, 0x08, pressed); break; // button1
        case SDL_SCANCODE_X:   set_bit(&s->in_p1, 0x10, pressed); break; // button2
        case SDL_SCANCODE_C:   set_bit(&s->in_p1, 0x01, pressed); break; // button3
        case SDL_SCANCODE_V:   set_bit(&s->in_p1, 0x04, pressed); break; // button4

        case SDL_SCANCODE_1:   set_bit(&s->in_sys, 0x04, pressed); break; // start1
        case SDL_SCANCODE_2:   set_bit(&s->in_sys, 0x08, pressed); break; // start2
        case SDL_SCANCODE_5:   set_bit(&s->in_sys, 0x01, pressed); break; // coin1
        case SDL_SCANCODE_6:   set_bit(&s->in_sys, 0x02, pressed); break; // coin2

        case SDL_SCANCODE_ESCAPE:
            if (pressed) s->quit = true;
            break;

        // Audio test tone (debug)
        case SDL_SCANCODE_F3:
            if (pressed) {
                s->audio_test_tone = !s->audio_test_tone;
                fprintf(stderr, "[AUDIO] test_tone=%d\n", s->audio_test_tone ? 1 : 0);
            }
            return;

        // Gain toggle
        case SDL_SCANCODE_F4:
            if (pressed) {
                s->audio_gain = (s->audio_gain < 0.9f) ? 1.0f : 0.25f;
                fprintf(stderr, "[AUDIO] gain=%.2f\n", s->audio_gain);
            }
            return;

		// Turbo
		case SDL_SCANCODE_F2:
			if (pressed) {
				s->turbo = !s->turbo;
				//if (!t->turbo_mode && t->audio_dev > 0) SDL_ClearQueuedAudio(t->audio_dev);
				printf("[EMU] Velocidad %s\n", s->turbo ? "MAXIMA" : "normal");
			}
			return;

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
    printf("  F3      = audio test tone (debug)\n");
    printf("  F4      = toggle gain 0.25/1.00\n");
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
            fprintf(stderr, "Opci n desconocida: %s\n", argv[i]);
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
