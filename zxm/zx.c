#include "zx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Instancia global
// -----------------------------------------------------------------------------
ZXSpectrum spec;

// Paleta de colores del ZX Spectrum (ARGB8888)
static const uint32_t palette[16] = {
    0xFF000000, 0xFF0000D7, 0xFFD70000, 0xFFD700D7,
    0xFF00D700, 0xFF00D7D7, 0xFFD7D700, 0xFFD7D7D7,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
};

// Tabla de volumen AY (aprox. logarítmica)
static const float ay_vol[16] = {
    0.0f, 0.004f, 0.006f, 0.009f,
    0.013f, 0.019f, 0.028f, 0.041f,
    0.060f, 0.088f, 0.129f, 0.189f,
    0.276f, 0.403f, 0.587f, 0.857f
};

// -----------------------------------------------------------------------------
// Helpers memoria paginada
// -----------------------------------------------------------------------------
static inline uint8_t* page_ptr(ZXSpectrum* s, uint16_t addr) {
    return s->mem_map[addr >> 14];
}

static inline uint8_t mem_peek(ZXSpectrum* s, uint16_t addr) {
    return page_ptr(s, addr)[addr & 0x3FFF];
}

// -----------------------------------------------------------------------------
// AY-3-8912
// -----------------------------------------------------------------------------
static void ay_reset(AYState* a) {
    memset(a, 0, sizeof(*a));
    a->lfsr = 0x1FFFF; // 17-bit
    a->noise_out = 1;
    for (int i = 0; i < 3; i++) {
        a->tone_out[i] = 1;
        a->tone_count[i] = 1;
        a->tone_period[i] = 1;
    }
    a->noise_count = 1;
    a->noise_period = 1;
    a->env_period = 1;
    a->env_count = 1;
    a->env_vol = 0;
    a->last_sample = 0.0f;
}

static void ay_env_restart(AYState* a, uint8_t shape) {
    a->env_continue = (shape >> 3) & 1;
    a->env_attack   = (shape >> 2) & 1;
    a->env_alt      = (shape >> 1) & 1;
    a->env_hold     = (shape >> 0) & 1;

    a->env_step = a->env_attack ? 1 : -1;
    a->env_vol  = a->env_attack ? 0 : 15;
    a->env_count = a->env_period ? a->env_period : 1;
}

static void ay_write_reg(AYState* a, uint8_t reg, uint8_t val) {
    reg &= 0x0F;

    static const uint8_t masks[16] = {
        0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF
    };
    val &= masks[reg];

    a->regs[reg] = val;

    a->tone_period[0] = (uint16_t)a->regs[0] | ((uint16_t)(a->regs[1] & 0x0F) << 8);
    a->tone_period[1] = (uint16_t)a->regs[2] | ((uint16_t)(a->regs[3] & 0x0F) << 8);
    a->tone_period[2] = (uint16_t)a->regs[4] | ((uint16_t)(a->regs[5] & 0x0F) << 8);
    if (a->tone_period[0] == 0) a->tone_period[0] = 1;
    if (a->tone_period[1] == 0) a->tone_period[1] = 1;
    if (a->tone_period[2] == 0) a->tone_period[2] = 1;

    a->noise_period = a->regs[6] & 0x1F;
    if (a->noise_period == 0) a->noise_period = 1;

    a->env_period = (uint16_t)a->regs[11] | ((uint16_t)a->regs[12] << 8);
    if (a->env_period == 0) a->env_period = 1;

    if (reg == 13) ay_env_restart(a, val);
}

static inline void ay_step_envelope(AYState* a) {
    int nv = (int)a->env_vol + (int)a->env_step;

    if (nv < 0 || nv > 15) {
        if (!a->env_continue) {
            a->env_vol = 0;
            a->env_step = 0;
            return;
        }
        if (a->env_hold) {
            a->env_vol = (a->env_step > 0) ? 15 : 0;
            a->env_step = 0;
            return;
        }
        if (a->env_alt) a->env_step = (int8_t)(-a->env_step);
        a->env_vol = (a->env_step > 0) ? 0 : 15;
        return;
    }

    a->env_vol = (uint8_t)nv;
}

static void ay_step_ticks(AYState* a, uint32_t ticks) {
    for (uint32_t t = 0; t < ticks; t++) {
        a->div16++;
        if (a->div16 >= 16) {
            a->div16 = 0;

            for (int ch = 0; ch < 3; ch++) {
                if (a->tone_count[ch] == 0) a->tone_count[ch] = a->tone_period[ch];
                a->tone_count[ch]--;
                if (a->tone_count[ch] == 0) {
                    a->tone_out[ch] ^= 1;
                    a->tone_count[ch] = a->tone_period[ch];
                }
            }

            if (a->noise_count == 0) a->noise_count = a->noise_period;
            a->noise_count--;
            if (a->noise_count == 0) {
                uint32_t bit = (a->lfsr ^ (a->lfsr >> 3)) & 1;
                a->lfsr = (a->lfsr >> 1) | (bit << 16);
                a->noise_out = (uint8_t)(a->lfsr & 1);
                a->noise_count = a->noise_period;
            }
        }

        a->div256++;
        if (a->div256 >= 256) {
            a->div256 = 0;
            if (a->env_count == 0) a->env_count = a->env_period;
            a->env_count--;
            if (a->env_count == 0) {
                a->env_count = a->env_period;
                ay_step_envelope(a);
            }
        }
    }
}

static void ay_step_tstates(AYState* a, uint32_t tstates) {
    uint64_t add = (uint64_t)tstates * (uint64_t)AY_CLOCK_HZ;
    uint64_t acc = (uint64_t)a->tick_accum + add;
    uint32_t ticks = (uint32_t)(acc / ZX_CPU_CLOCK_HZ);
    a->tick_accum = (uint32_t)(acc % ZX_CPU_CLOCK_HZ);
    if (ticks) ay_step_ticks(a, ticks);
}

static float ay_mix(AYState* a) {
    uint8_t mixer = a->regs[7];

    float sum = 0.0f;
    for (int ch = 0; ch < 3; ch++) {
        int tone_en  = ((mixer >> ch) & 1) ? 0 : 1;
        int noise_en = ((mixer >> (ch + 3)) & 1) ? 0 : 1;

        int tone_ok  = tone_en  ? a->tone_out[ch] : 1;
        int noise_ok = noise_en ? a->noise_out    : 1;

        uint8_t vr = a->regs[8 + ch];
        uint8_t v = (vr & 0x10) ? a->env_vol : (vr & 0x0F);
        float amp = ay_vol[v & 0x0F];

        float s = (tone_ok && noise_ok) ? amp : 0.0f;
        sum += s;
    }

    return (sum / 3.0f) * 0.9f;
}

// -----------------------------------------------------------------------------
// Paginación 48/128/+3
// -----------------------------------------------------------------------------
static inline uint8_t plus3_rom_index(uint8_t port7ffd, uint8_t port1ffd) {
    uint8_t lo = (port7ffd >> 4) & 1; // ROM low bit
    uint8_t hi = (port1ffd >> 2) & 1; // ROM high bit
    return (uint8_t)((hi << 1) | lo);
}

static inline void spectrum_update_memory_map(ZXSpectrum* s) {
    // Pantalla visible (bit3 de 7FFD)
    s->screen_bank = (s->port_7ffd & 0x08) ? 7 : 5;
    s->screen_ptr  = s->ram[s->screen_bank];

    if (s->model == ZX_MODEL_48K) {
        s->mem_map[0] = s->rom48;
        s->mem_map[1] = s->ram[5];
        s->mem_map[2] = s->ram[2];
        s->mem_map[3] = s->ram[s->bank_c000 & 7];
        return;
    }

    if (s->model == ZX_MODEL_128K) {
        s->mem_map[1] = s->ram[5];
        s->mem_map[2] = s->ram[2];
        s->mem_map[3] = s->ram[s->bank_c000 & 7];
        s->mem_map[0] = s->have_rom128 ? s->rom128[s->rom_page & 1] : s->rom48;
        return;
    }

    // ZX_MODEL_PLUS3
    if (s->special_paging) {
        uint8_t mode = (s->port_1ffd >> 1) & 0x03;
        switch (mode) {
            case 0: s->mem_map[0]=s->ram[0]; s->mem_map[1]=s->ram[1]; s->mem_map[2]=s->ram[2]; s->mem_map[3]=s->ram[3]; break;
            case 1: s->mem_map[0]=s->ram[4]; s->mem_map[1]=s->ram[5]; s->mem_map[2]=s->ram[6]; s->mem_map[3]=s->ram[7]; break;
            case 2: s->mem_map[0]=s->ram[4]; s->mem_map[1]=s->ram[5]; s->mem_map[2]=s->ram[6]; s->mem_map[3]=s->ram[3]; break;
            default:s->mem_map[0]=s->ram[4]; s->mem_map[1]=s->ram[7]; s->mem_map[2]=s->ram[6]; s->mem_map[3]=s->ram[3]; break;
        }
    } else {
        s->mem_map[1] = s->ram[5];
        s->mem_map[2] = s->ram[2];
        s->mem_map[3] = s->ram[s->bank_c000 & 7];
        if (s->have_rom_plus3) s->mem_map[0] = s->rom_plus3[s->rom_page & 3];
        else if (s->have_rom128) s->mem_map[0] = s->rom128[s->rom_page & 1];
        else s->mem_map[0] = s->rom48;
    }
}

static inline void spectrum_apply_7ffd_force(ZXSpectrum* s, uint8_t val) {
    s->port_7ffd = val;
    s->bank_c000 = val & 0x07;
    if (val & 0x20) s->paging_lock = true;

    if (s->model == ZX_MODEL_128K) {
        s->rom_page = (val & 0x10) ? 1 : 0;
    } else if (s->model == ZX_MODEL_PLUS3) {
        s->rom_page = plus3_rom_index(s->port_7ffd, s->port_1ffd);
    } else {
        s->rom_page = 0;
    }

    spectrum_update_memory_map(s);
}

static inline void spectrum_write_7ffd(ZXSpectrum* s, uint8_t val) {
    if (s->model == ZX_MODEL_48K) return;
    if (s->paging_lock) return;
    spectrum_apply_7ffd_force(s, val);
}

static inline void spectrum_apply_1ffd_force(ZXSpectrum* s, uint8_t val) {
    s->port_1ffd = val;
    s->special_paging = (val & 0x01) ? true : false;

    // ROM high bit + ROM low bit
    s->rom_page = plus3_rom_index(s->port_7ffd, s->port_1ffd);

    // Motor disquetera: bit3 -> control al FDC (bit0=A, bit1=B)
    uint8_t motor = (val & 0x08) ? 0x03 : 0x00;
    fdc_motor_control(&s->fdc, motor);

    spectrum_update_memory_map(s);
}

static inline void spectrum_write_1ffd(ZXSpectrum* s, uint8_t val) {
    if (s->model != ZX_MODEL_PLUS3) return;
    if (s->paging_lock) return;
    spectrum_apply_1ffd_force(s, val);
}

// -----------------------------------------------------------------------------
// Contención de memoria (aprox)
// -----------------------------------------------------------------------------
static inline bool bank_is_contended(ZXSpectrum* s, int bank) {
    if (s->model == ZX_MODEL_PLUS3) return (bank >= 4 && bank <= 7);
    return (bank == 5 || bank == 7);
}

static inline bool addr_contended(ZXSpectrum* s, uint16_t addr) {
    int page = addr >> 14;
    if (s->model == ZX_MODEL_48K) return (page == 1);
    if (page == 1) return true;
    if (page == 3) return bank_is_contended(s, s->bank_c000);
    return false;
}

static void ula_setup_clash(ZXSpectrum* s) {
    memset(s->ula_clash, 0, sizeof(s->ula_clash));
    static const uint8_t pattern[8] = {6,5,4,3,2,1,0,0};

    int j = (s->model == ZX_MODEL_PLUS3) ? 14365 : 14335;
    for (int y = 0; y < 192; y++) {
        for (int x = 0; x < 128; x++) {
            int idx = j++;
            if (idx >= 0 && idx < s->ula_ticks_per_frame)
                s->ula_clash[idx] = pattern[x & 7];
        }
        j += s->ula_tstates_per_line - 128;
    }
}

// -----------------------------------------------------------------------------
// Floating bus
// -----------------------------------------------------------------------------
static int ula_recv_floating(ZXSpectrum* s) {
    if (s->ula_shown_y >= 0 && s->ula_shown_y < 192 && s->ula_shown_x >= 0 && s->ula_shown_x < 32) {
        int col = s->ula_shown_x;
        switch (s->ula_count_z & 3) {
            case 0: return s->screen_ptr[s->ula_bitmap + col];
            case 1: return s->screen_ptr[s->ula_attrib + col];
            case 2: return (col < 31) ? s->screen_ptr[s->ula_bitmap + col + 1] : 0xFF;
            default:return (col < 31) ? s->screen_ptr[s->ula_attrib + col + 1] : 0xFF;
        }
    }
    return 0xFF;
}

// -----------------------------------------------------------------------------
// Renderizado ULA por T-state
// -----------------------------------------------------------------------------
static void ula_video_step(ZXSpectrum* s) {
    if (s->ula_count_x == 38) s->video_y++;

    int fb_y = s->video_y;
    bool visible_y = (fb_y >= 0 && fb_y < FULL_H);

    int vis_char = (s->ula_count_x + 6) % s->ula_chars_per_line;
    bool visible_x = (vis_char < 44);

    bool in_paper = (s->ula_count_x < 32 && s->ula_shown_y >= 0 && s->ula_shown_y < 192);

    uint8_t bitmap = 0, attrib = 0;

    if (in_paper) {
        int col = s->ula_count_x;
        bitmap = s->screen_ptr[s->ula_bitmap + col];
        attrib = s->screen_ptr[s->ula_attrib + col];
        s->ula_bus = attrib;
    } else {
        s->ula_bus = -1;
    }

    if (visible_y && visible_x) {
        int fb_x = vis_char * 8;
        uint32_t* dst = &s->framebuffer[fb_y * FULL_W + fb_x];

        if (in_paper) {
            bool bright = (attrib & 0x40) != 0;
            bool flash  = (attrib & 0x80) != 0;
            int ink   = (attrib & 0x07) | (bright ? 8 : 0);
            int paper = ((attrib >> 3) & 0x07) | (bright ? 8 : 0);
            if (flash && (s->frame_counter & 16)) { int t=ink; ink=paper; paper=t; }
            uint32_t ci = palette[ink], cp = palette[paper];

            dst[0] = (bitmap & 0x80) ? ci : cp;
            dst[1] = (bitmap & 0x40) ? ci : cp;
            dst[2] = (bitmap & 0x20) ? ci : cp;
            dst[3] = (bitmap & 0x10) ? ci : cp;
            dst[4] = (bitmap & 0x08) ? ci : cp;
            dst[5] = (bitmap & 0x04) ? ci : cp;
            dst[6] = (bitmap & 0x02) ? ci : cp;
            dst[7] = (bitmap & 0x01) ? ci : cp;
        } else {
            uint32_t bc = palette[s->border_color & 0x07];
            dst[0]=bc; dst[1]=bc; dst[2]=bc; dst[3]=bc;
            dst[4]=bc; dst[5]=bc; dst[6]=bc; dst[7]=bc;
        }
    }

    s->ula_shown_x++;

    if (++s->ula_count_x >= s->ula_chars_per_line) {
        s->ula_count_x = 0;
        s->ula_shown_x = 0;
        s->ula_count_y++;
        s->ula_shown_y++;

        if (s->ula_shown_y >= 0 && s->ula_shown_y < 192) {
            int y = s->ula_shown_y;
            s->ula_bitmap = ((y & 0xC0) << 5) | ((y & 7) << 8) | (((y >> 3) & 7) << 5);
            s->ula_attrib = 0x1800 + ((y >> 3) << 5);
        }

        s->ula_snow_a = 0; // mantenemos snow deshabilitado en este core
    }
}

static void ula_video_main(ZXSpectrum* s, int t) {
    s->ula_count_z += t;
    while (s->ula_count_z >= 4) {
        s->ula_count_z -= 4;
        ula_video_step(s);
    }
}

// -----------------------------------------------------------------------------
// Callbacks de Memoria e I/O
// -----------------------------------------------------------------------------
static uint8_t mem_read(void* userdata, uint16_t addr) {
    ZXSpectrum* s = (ZXSpectrum*)userdata;

    if (addr_contended(s, addr)) {
        int pos = s->ula_clash_z % s->ula_ticks_per_frame;
        int delay = s->ula_clash[pos];
        s->ula_clash_z += delay;
        s->contention_extra += delay;
    }

    return page_ptr(s, addr)[addr & 0x3FFF];
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    ZXSpectrum* s = (ZXSpectrum*)userdata;

    if (addr_contended(s, addr)) {
        int pos = s->ula_clash_z % s->ula_ticks_per_frame;
        int delay = s->ula_clash[pos];
        s->ula_clash_z += delay;
        s->contention_extra += delay;
    }

    if (addr < 0x4000) return;
    page_ptr(s, addr)[addr & 0x3FFF] = val;
}

static inline int is_7ffd(uint16_t port) {
    // 128K: cualquier puerto con bit1=0 y bit15=0; usamos máscara estándar
    return ((port & 0x8002) == 0x0000) && ((port & 0x7FFD) == 0x7FFD);
}

static inline int is_fffd(uint16_t port) {
    return ((port & 0xC002) == 0xC000);
}

static inline int is_bffd(uint16_t port) {
    return ((port & 0xC002) == 0x8000);
}

static inline int is_1ffd(uint16_t port) { return (port == 0x1FFD); }
static inline int is_2ffd(uint16_t port) { return (port == 0x2FFD); }
static inline int is_3ffd(uint16_t port) { return (port == 0x3FFD); }

static uint8_t port_in(z80* z, uint16_t port) {
    ZXSpectrum* s = (ZXSpectrum*)z->userdata;

    if (!(port & 1) || addr_contended(s, port)) {
        int pos = s->ula_clash_z % s->ula_ticks_per_frame;
        int delay = s->ula_clash[pos];
        s->ula_clash_z += delay;
        s->contention_extra += delay;
    }

    // ULA 0xFE
    if ((port & 0x01) == 0) {
        uint8_t result = 0xFF;
        for (int i = 0; i < 8; i++)
            if ((port & (1 << (i + 8))) == 0)
                result &= s->keyboard_matrix[i];

        uint8_t ear_in = s->ear_bit | s->mic_bit;
        if (ear_in) result |= 0x40; else result &= ~0x40;
        return result;
    }

    // AY lectura
    if ((s->model == ZX_MODEL_128K || s->model == ZX_MODEL_PLUS3) && is_fffd(port)) {
        return s->ay.regs[s->ay.sel & 0x0F];
    }

    // +3 FDC
    if (s->model == ZX_MODEL_PLUS3) {
        if (is_2ffd(port)) return fdc_read_status(&s->fdc);
        if (is_3ffd(port)) return fdc_read_data(&s->fdc);
    }

    // Kempston
    if ((port & 0xE0) == 0) return s->kempston;

    // Floating bus
    return (uint8_t)(ula_recv_floating(s) & 0xFF);
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    ZXSpectrum* s = (ZXSpectrum*)z->userdata;

    if (!(port & 1) || addr_contended(s, port)) {
        int pos = s->ula_clash_z % s->ula_ticks_per_frame;
        int delay = s->ula_clash[pos];
        s->ula_clash_z += delay;
        s->contention_extra += delay;
    }

    // ULA 0xFE
    if ((port & 0x01) == 0) {
        s->border_color = val & 0x07;
        s->ear_bit      = (val & 0x10) >> 4;
        return;
    }

    // 7FFD
    if ((s->model == ZX_MODEL_128K || s->model == ZX_MODEL_PLUS3) && is_7ffd(port)) {
        spectrum_write_7ffd(s, val);
        return;
    }

    // 1FFD (+3)
    if (s->model == ZX_MODEL_PLUS3 && is_1ffd(port)) {
        spectrum_write_1ffd(s, val);
        return;
    }

    // AY
    if (s->model == ZX_MODEL_128K || s->model == ZX_MODEL_PLUS3) {
        if (is_fffd(port)) { s->ay.sel = val & 0x0F; return; }
        if (is_bffd(port)) { ay_write_reg(&s->ay, s->ay.sel, val); return; }
    }

    // FDC data write
    if (s->model == ZX_MODEL_PLUS3 && is_3ffd(port)) {
        fdc_write_data(&s->fdc, val);
        return;
    }
}

// -----------------------------------------------------------------------------
// Reproductor TAP por pulsos
// -----------------------------------------------------------------------------
int spectrum_load_tap(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    free(s->tap.data);
    s->tap.data = (uint8_t*)malloc((size_t)sz);
    if (!s->tap.data) { fclose(f); return -1; }

    fread(s->tap.data, 1, (size_t)sz, f);
    fclose(f);

    s->tap.size = (uint32_t)sz;
    s->tap.pos = 0;
    s->tap.active = false;
    s->tap.state = TAP_STATE_IDLE;
    s->tap.ear = 0;
    s->tape_src = TAPE_TAP;
    s->mic_bit = 0;

    printf("[TAP] Fichero cargado: %s (%ld bytes)\n", filename, sz);
    return 0;
}

static bool tap_next_block(TAPPlayer* t) {
    if (!t->data || t->pos + 2 > t->size) {
        t->state = TAP_STATE_IDLE;
        t->active = false;
        printf("[TAP] Fin de cinta.\n");
        return false;
    }

    t->block_len = (uint32_t)t->data[t->pos] | ((uint32_t)t->data[t->pos + 1] << 8);
    t->pos += 2;

    if (t->pos + t->block_len > t->size) {
        t->state = TAP_STATE_IDLE;
        t->active = false;
        printf("[TAP] Bloque truncado.\n");
        return false;
    }

    t->byte_pos = 0;
    t->bit_mask = 0x80;

    uint8_t flag = t->data[t->pos];
    t->pilot_count = (flag == 0x00) ? TAP_PILOT_HEADER : TAP_PILOT_DATA;
    t->state = TAP_STATE_PILOT;
    t->pulse_cycles = TAP_PILOT_PULSE;
    t->ear = 1;

    printf("[TAP] Bloque %u bytes, tipo %s\n", t->block_len,
           (flag == 0x00) ? "cabecera" : "datos");
    return true;
}

static void tap_update(TAPPlayer* t, int cycles) {
    if (!t->active || t->state == TAP_STATE_IDLE) return;

    t->pulse_cycles -= cycles;
    while (t->pulse_cycles <= 0) {
        t->ear ^= 1;

        switch (t->state) {
            case TAP_STATE_PILOT:
                t->pilot_count--;
                if (t->pilot_count <= 0) {
                    t->state = TAP_STATE_SYNC1;
                    t->pulse_cycles += TAP_SYNC1_PULSE;
                } else {
                    t->pulse_cycles += TAP_PILOT_PULSE;
                }
                break;

            case TAP_STATE_SYNC1:
                t->state = TAP_STATE_SYNC2;
                t->pulse_cycles += TAP_SYNC2_PULSE;
                break;

            case TAP_STATE_SYNC2:
                t->state = TAP_STATE_DATA;
                t->byte_pos = 0;
                t->bit_mask = 0x80;
                t->pulse_cycles += (t->data[t->pos + t->byte_pos] & t->bit_mask) ?
                                    TAP_BIT1_PULSE : TAP_BIT0_PULSE;
                break;

            case TAP_STATE_DATA: {
                uint8_t cur_byte = t->data[t->pos + t->byte_pos];
                int pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;

                if (t->ear == 0) {
                    t->bit_mask >>= 1;
                    if (t->bit_mask == 0) {
                        t->bit_mask = 0x80;
                        t->byte_pos++;
                        if (t->byte_pos >= t->block_len) {
                            t->pos += t->block_len;
                            t->state = TAP_STATE_PAUSE;
                            t->pulse_cycles += TAP_PAUSE_CYCLES;
                            break;
                        }
                        cur_byte = t->data[t->pos + t->byte_pos];
                    }
                    pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
                }

                t->pulse_cycles += pw;
                break;
            }

            case TAP_STATE_PAUSE:
                t->ear = 0;
                if (!tap_next_block(t)) return;
                break;

            default:
                return;
        }
    }
}

static void tap_start(TAPPlayer* t) {
    if (!t->data || t->size == 0) return;
    t->pos = 0;
    t->active = true;
    printf("[TAP] Reproducción iniciada.\n");
    tap_next_block(t);
}

// -----------------------------------------------------------------------------
// TZX (delegada en tzx.c)
// -----------------------------------------------------------------------------
int spectrum_load_tzx(ZXSpectrum* s, const char* filename) {
    s->tap.active = false;
    tzx_free(&s->tzx);
    if (tzx_load(&s->tzx, filename) != 0)
        return -1;
    s->tape_src = TAPE_TZX;
    s->mic_bit = 0;
    return 0;
}

void spectrum_tape_start(ZXSpectrum* s) {
    switch (s->tape_src) {
        case TAPE_TAP: tap_start(&s->tap); break;
        case TAPE_TZX: tzx_start(&s->tzx); break;
        default: printf("[TAPE] No hay cinta cargada.\n"); break;
    }
}

// -----------------------------------------------------------------------------
// Inicialización / destrucción
// -----------------------------------------------------------------------------
static void spectrum_set_model_defaults(ZXSpectrum* s, ZXModel model) {
    s->model = model;

    if (model == ZX_MODEL_48K) {
        s->ula_tstates_per_line = 224;
        s->ula_lines_per_frame  = 312;
        s->ula_ticks_per_frame  = 69888;
        s->ula_chars_per_line   = 56;
    } else {
        s->ula_tstates_per_line = 228;
        s->ula_lines_per_frame  = 311;
        s->ula_ticks_per_frame  = 70908;
        s->ula_chars_per_line   = 57;
    }

    s->port_7ffd = 0x00;
    s->port_1ffd = 0x00;
    s->paging_lock = false;
    s->special_paging = false;
    s->bank_c000 = 0;
    s->rom_page = 0;

    spectrum_update_memory_map(s);
    ula_setup_clash(s);

    ay_reset(&s->ay);
    fdc_init(&s->fdc);
}

void spectrum_init(ZXSpectrum* s, ZXModel model) {
    memset(s, 0, sizeof(ZXSpectrum));
    for (int i = 0; i < 8; i++) s->keyboard_matrix[i] = 0xFF;
    s->tape_src = TAPE_NONE;
    s->ula_snow_disabled = 1;
    s->ula_bus = -1;

    spectrum_set_model_defaults(s, model);

    z80_init(&s->cpu);
    s->cpu.userdata   = s;
    s->cpu.read_byte  = mem_read;
    s->cpu.write_byte = mem_write;
    s->cpu.port_in    = port_in;
    s->cpu.port_out   = port_out;
    z80_reset(&s->cpu);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    const char* title = (model == ZX_MODEL_PLUS3) ? "ZX Spectrum +3" :
                        (model == ZX_MODEL_128K) ? "ZX Spectrum 128K" : "ZX Spectrum 48K";

    s->window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        FULL_W * SCALE, FULL_H * SCALE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    s->renderer = SDL_CreateRenderer(s->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    s->texture = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, FULL_W, FULL_H);

    SDL_RenderSetLogicalSize(s->renderer, FULL_W * SCALE, FULL_H * SCALE);

    SDL_AudioSpec wanted, have;
    SDL_zero(wanted);
    wanted.freq = AUDIO_HZ;
    wanted.format = AUDIO_F32;
    wanted.channels = 1;
    wanted.samples = 1024;

    s->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &have, 0);
    if (s->audio_dev > 0) SDL_PauseAudioDevice(s->audio_dev, 0);
}

void spectrum_destroy(ZXSpectrum* s) {
    free(s->tap.data);
    s->tap.data = NULL;
    tzx_free(&s->tzx);

    SDL_DestroyTexture(s->texture);
    SDL_DestroyRenderer(s->renderer);
    SDL_DestroyWindow(s->window);
    SDL_CloseAudioDevice(s->audio_dev);
    SDL_Quit();
}

// -----------------------------------------------------------------------------
// ROMs
// -----------------------------------------------------------------------------
int spectrum_load_rom48(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    size_t n = fread(s->rom48, 1, 16384, f);
    fclose(f);
    if (n != 16384) return -1;
    s->have_rom48 = true;
    if (s->model == ZX_MODEL_48K) spectrum_update_memory_map(s);
    return 0;
}

int spectrum_load_rom128(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != 32768) { fclose(f); return -1; }

    size_t n0 = fread(s->rom128[0], 1, 16384, f);
    size_t n1 = fread(s->rom128[1], 1, 16384, f);
    fclose(f);

    if (n0 != 16384 || n1 != 16384) return -1;
    s->have_rom128 = true;
    if (s->model == ZX_MODEL_128K) spectrum_update_memory_map(s);
    return 0;
}

static int load16k(uint8_t* dst, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(dst, 1, 16384, f);
    fclose(f);
    return (n == 16384) ? 0 : -1;
}

int spectrum_load_rom_plus3_set(ZXSpectrum* s,
                               const char* rom0,
                               const char* rom1,
                               const char* rom2,
                               const char* rom3) {
    if (load16k(s->rom_plus3[0], rom0) != 0) return -1;
    if (load16k(s->rom_plus3[1], rom1) != 0) return -1;
    if (load16k(s->rom_plus3[2], rom2) != 0) return -1;
    if (load16k(s->rom_plus3[3], rom3) != 0) return -1;

    s->have_rom_plus3 = true;
    if (s->model == ZX_MODEL_PLUS3) spectrum_update_memory_map(s);

    printf("[ROM +3] Cargadas: %s, %s, %s, %s\n", rom0, rom1, rom2, rom3);
    return 0;
}

// -----------------------------------------------------------------------------
// SNA 48K y 128K
// -----------------------------------------------------------------------------
static int spectrum_load_sna48(ZXSpectrum* s, FILE* f) {
    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) return -1;

    uint8_t buf[49152];
    if (fread(buf, 1, 49152, f) != 49152) return -1;

    memcpy(s->ram[5], buf + 0x0000, 16384);
    memcpy(s->ram[2], buf + 0x4000, 16384);
    memcpy(s->ram[0], buf + 0x8000, 16384);

    spectrum_set_model_defaults(s, ZX_MODEL_48K);
    if (s->window) SDL_SetWindowTitle(s->window, "ZX Spectrum 48K");

    s->border_color = header[26] & 7;

    // Registros CPU
    s->cpu.i  = header[0];
    s->cpu.l_ = header[1];  s->cpu.h_ = header[2];
    s->cpu.e_ = header[3];  s->cpu.d_ = header[4];
    s->cpu.c_ = header[5];  s->cpu.b_ = header[6];
    s->cpu.f_ = header[7];  s->cpu.a_ = header[8];
    s->cpu.l  = header[9];  s->cpu.h  = header[10];
    s->cpu.e  = header[11]; s->cpu.d  = header[12];
    s->cpu.c  = header[13]; s->cpu.b  = header[14];
    s->cpu.iy = (uint16_t)header[15] | ((uint16_t)header[16] << 8);
    s->cpu.ix = (uint16_t)header[17] | ((uint16_t)header[18] << 8);

    uint8_t iff2 = (header[19] & 0x04) != 0;
    s->cpu.iff2 = iff2;
    s->cpu.iff1 = iff2;
    s->cpu.r = header[20];

    s->cpu.f  = header[21];
    s->cpu.a  = header[22];
    s->cpu.sp = (uint16_t)header[23] | ((uint16_t)header[24] << 8);
    s->cpu.interrupt_mode = header[25];

    uint16_t sp = s->cpu.sp;
    uint16_t pc = (uint16_t)mem_peek(s, sp) | ((uint16_t)mem_peek(s, sp + 1) << 8);
    s->cpu.pc = pc;
    s->cpu.sp += 2;

    return 0;
}

static int spectrum_load_sna128(ZXSpectrum* s, FILE* f) {
    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) return -1;

    uint8_t buf[49152];
    if (fread(buf, 1, 49152, f) != 49152) return -1;

    uint8_t tail[4];
    if (fread(tail, 1, 4, f) != 4) return -1;

    uint16_t pc = (uint16_t)tail[0] | ((uint16_t)tail[1] << 8);
    uint8_t port7ffd = tail[2];

    spectrum_set_model_defaults(s, ZX_MODEL_128K);
    if (s->window) SDL_SetWindowTitle(s->window, "ZX Spectrum 128K");

    uint8_t paged = port7ffd & 0x07;

    memcpy(s->ram[5], buf + 0x0000, 16384);
    memcpy(s->ram[2], buf + 0x4000, 16384);
    memcpy(s->ram[paged], buf + 0x8000, 16384);

    for (int b = 0; b < 8; b++) {
        if (b == 5 || b == 2 || b == paged) continue;
        if (fread(s->ram[b], 1, 16384, f) != 16384) return -1;
    }

    spectrum_apply_7ffd_force(s, port7ffd);

    s->border_color = header[26] & 7;

    // Registros CPU
    s->cpu.i  = header[0];
    s->cpu.l_ = header[1];  s->cpu.h_ = header[2];
    s->cpu.e_ = header[3];  s->cpu.d_ = header[4];
    s->cpu.c_ = header[5];  s->cpu.b_ = header[6];
    s->cpu.f_ = header[7];  s->cpu.a_ = header[8];
    s->cpu.l  = header[9];  s->cpu.h  = header[10];
    s->cpu.e  = header[11]; s->cpu.d  = header[12];
    s->cpu.c  = header[13]; s->cpu.b  = header[14];
    s->cpu.iy = (uint16_t)header[15] | ((uint16_t)header[16] << 8);
    s->cpu.ix = (uint16_t)header[17] | ((uint16_t)header[18] << 8);

    uint8_t iff2 = (header[19] & 0x04) != 0;
    s->cpu.iff2 = iff2;
    s->cpu.iff1 = iff2;
    s->cpu.r = header[20];

    s->cpu.f  = header[21];
    s->cpu.a  = header[22];
    s->cpu.sp = (uint16_t)header[23] | ((uint16_t)header[24] << 8);
    s->cpu.interrupt_mode = header[25];

    s->cpu.pc = pc;

    return 0;
}

int spectrum_load_sna(ZXSpectrum* s, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    int rc = -1;
    if (sz == 49179) rc = spectrum_load_sna48(s, f);
    else if (sz == 131103) rc = spectrum_load_sna128(s, f);
    else {
        printf("[SNA] Tamaño no reconocido: %ld (esperado 49179 o 131103)\n", sz);
        rc = -1;
    }

    fclose(f);
    return rc;
}

// -----------------------------------------------------------------------------
// Teclado
// -----------------------------------------------------------------------------
void spectrum_handle_key(ZXSpectrum* s, SDL_Scancode key, bool pressed) {
    uint8_t row = 255, bit = 255;

    switch (key) {
        case SDL_SCANCODE_LSHIFT: row=0; bit=0; break;
        case SDL_SCANCODE_Z:      row=0; bit=1; break;
        case SDL_SCANCODE_X:      row=0; bit=2; break;
        case SDL_SCANCODE_C:      row=0; bit=3; break;
        case SDL_SCANCODE_V:      row=0; bit=4; break;

        case SDL_SCANCODE_A:      row=1; bit=0; break;
        case SDL_SCANCODE_S:      row=1; bit=1; break;
        case SDL_SCANCODE_D:      row=1; bit=2; break;
        case SDL_SCANCODE_F:      row=1; bit=3; break;
        case SDL_SCANCODE_G:      row=1; bit=4; break;

        case SDL_SCANCODE_Q:      row=2; bit=0; break;
        case SDL_SCANCODE_W:      row=2; bit=1; break;
        case SDL_SCANCODE_E:      row=2; bit=2; break;
        case SDL_SCANCODE_R:      row=2; bit=3; break;
        case SDL_SCANCODE_T:      row=2; bit=4; break;

        case SDL_SCANCODE_1:      row=3; bit=0; break;
        case SDL_SCANCODE_2:      row=3; bit=1; break;
        case SDL_SCANCODE_3:      row=3; bit=2; break;
        case SDL_SCANCODE_4:      row=3; bit=3; break;
        case SDL_SCANCODE_5:      row=3; bit=4; break;

        case SDL_SCANCODE_0:      row=4; bit=0; break;
        case SDL_SCANCODE_9:      row=4; bit=1; break;
        case SDL_SCANCODE_8:      row=4; bit=2; break;
        case SDL_SCANCODE_7:      row=4; bit=3; break;
        case SDL_SCANCODE_6:      row=4; bit=4; break;

        case SDL_SCANCODE_P:      row=5; bit=0; break;
        case SDL_SCANCODE_O:      row=5; bit=1; break;
        case SDL_SCANCODE_I:      row=5; bit=2; break;
        case SDL_SCANCODE_U:      row=5; bit=3; break;
        case SDL_SCANCODE_Y:      row=5; bit=4; break;

        case SDL_SCANCODE_RETURN: row=6; bit=0; break;
        case SDL_SCANCODE_L:      row=6; bit=1; break;
        case SDL_SCANCODE_K:      row=6; bit=2; break;
        case SDL_SCANCODE_J:      row=6; bit=3; break;
        case SDL_SCANCODE_H:      row=6; bit=4; break;

        case SDL_SCANCODE_SPACE:  row=7; bit=0; break;
        case SDL_SCANCODE_RSHIFT: row=7; bit=1; break;
        case SDL_SCANCODE_M:      row=7; bit=2; break;
        case SDL_SCANCODE_N:      row=7; bit=3; break;
        case SDL_SCANCODE_B:      row=7; bit=4; break;

        // Kempston joystick
        case SDL_SCANCODE_RIGHT:
            if (pressed) s->kempston |= 0x01; else s->kempston &= ~0x01; return;
        case SDL_SCANCODE_LEFT:
            if (pressed) s->kempston |= 0x02; else s->kempston &= ~0x02; return;
        case SDL_SCANCODE_DOWN:
            if (pressed) s->kempston |= 0x04; else s->kempston &= ~0x04; return;
        case SDL_SCANCODE_UP:
            if (pressed) s->kempston |= 0x08; else s->kempston &= ~0x08; return;
        case SDL_SCANCODE_RALT:
        case SDL_SCANCODE_RCTRL:
            if (pressed) s->kempston |= 0x10; else s->kempston &= ~0x10; return;

        // Tape
        case SDL_SCANCODE_F1:
            if (pressed) spectrum_tape_start(s);
            return;

        // Turbo
        case SDL_SCANCODE_F2:
            if (pressed) {
                s->turbo_mode = !s->turbo_mode;
                if (!s->turbo_mode && s->audio_dev > 0) SDL_ClearQueuedAudio(s->audio_dev);
                printf("[EMU] Velocidad %s\n", s->turbo_mode ? "MAXIMA" : "normal");
            }
            return;

        // Stop tape
        case SDL_SCANCODE_F3:
            if (pressed) {
                s->tap.active = false;
                s->tzx.active = false;
                s->mic_bit = 0;
                printf("[TAPE] Cinta detenida.\n");
            }
            return;

        default:
            break;
    }

    if (row != 255) {
        if (pressed) s->keyboard_matrix[row] &= ~(1 << bit);
        else         s->keyboard_matrix[row] |=  (1 << bit);
    }
}

// -----------------------------------------------------------------------------
// Bucle de frame
// -----------------------------------------------------------------------------
void spectrum_run_frame(ZXSpectrum* s) {
    const int TSTATES_PER_FRAME  = s->ula_ticks_per_frame;
    const int TSTATES_PER_SAMPLE = TSTATES_PER_FRAME / AUDIO_SAMPLES_PER_FRAME;

    int cycles_done = 0;
    int next_sample_at = TSTATES_PER_SAMPLE;
    int prev_sample_at = 0;

    s->ula_count_x = 0;
    s->ula_count_y = 0;
    s->ula_count_z = 0;
    s->ula_clash_z = 0;
    s->ula_shown_x = 0;
    s->ula_shown_y = -ULA_FIRST_PAPER_LINE;
    s->video_y     = -ULA_FIRST_VISIBLE_LINE;
    s->ula_bus     = -1;
    s->ula_bitmap  = 0;
    s->ula_attrib  = 0x1800;

    z80_pulse_irq(&s->cpu, 0xFF);

    while (cycles_done < TSTATES_PER_FRAME) {
        s->contention_extra = 0;
        int base_cycles = (int)z80_step(&s->cpu);
        int total = base_cycles + s->contention_extra;

        ula_video_main(s, total);
        s->ula_clash_z += base_cycles;
        cycles_done += total;

        // Tape
        switch (s->tape_src) {
            case TAPE_TAP:
                tap_update(&s->tap, total);
                s->mic_bit = s->tap.ear;
                break;
            case TAPE_TZX:
                tzx_update(&s->tzx, total);
                s->mic_bit = s->tzx.ear;
                break;
            default:
                break;
        }

        // FDC
        if (s->model == ZX_MODEL_PLUS3) {
            fdc_tick(&s->fdc, total);
            if (fdc_irq(&s->fdc)) {
                z80_pulse_irq(&s->cpu, 0xFF);
            }
        }

        // Audio
        while (cycles_done >= next_sample_at) {
            if (s->audio_pos < AUDIO_SAMPLES_PER_FRAME) {
                int delta_t = next_sample_at - prev_sample_at;
                prev_sample_at = next_sample_at;

                if (s->model == ZX_MODEL_128K || s->model == ZX_MODEL_PLUS3)
                    ay_step_tstates(&s->ay, (uint32_t)delta_t);

                float ay_out = (s->model == ZX_MODEL_128K || s->model == ZX_MODEL_PLUS3) ? ay_mix(&s->ay) : 0.0f;

                float level = 0.0f;
                if (s->ear_bit) level += 0.12f;
                if (s->mic_bit) level += 0.12f;

                level += (ay_out - 0.15f);
                if (level == 0.0f) level = -0.05f;
                if (level > 1.0f) level = 1.0f;
                if (level < -1.0f) level = -1.0f;

                s->audio_buffer[s->audio_pos++] = level;
            }
            next_sample_at += TSTATES_PER_SAMPLE;
        }
    }

    if (s->audio_dev > 0 && s->audio_pos > 0) {
        if (!s->turbo_mode)
            SDL_QueueAudio(s->audio_dev, s->audio_buffer, s->audio_pos * sizeof(float));
        s->audio_pos = 0;
    }

    s->frame_counter++;
}

// -----------------------------------------------------------------------------
// Presentación SDL
// -----------------------------------------------------------------------------
void spectrum_render(ZXSpectrum* s) {
    SDL_UpdateTexture(s->texture, NULL, s->framebuffer, FULL_W * (int)sizeof(uint32_t));
    SDL_RenderClear(s->renderer);
    SDL_Rect dst_rect = { 0, 0, FULL_W * SCALE, FULL_H * SCALE };
    SDL_RenderCopy(s->renderer, s->texture, NULL, &dst_rect);
    SDL_RenderPresent(s->renderer);
}

// -----------------------------------------------------------------------------
// Utilidades
// -----------------------------------------------------------------------------
static bool ext_eq(const char* path, const char* ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char* tail = path + plen - elen;

    for (size_t i = 0; i < elen; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static void print_usage(const char* exe) {
    printf("Uso: %s [--48k | --128k | --plus3] <archivo.sna|archivo.tap|archivo.tzx|archivo.dsk>...\n", exe);
    printf("  --48k    ZX Spectrum 48K (por defecto)\n");
    printf("  --128k   ZX Spectrum 128K (paginación 0x7FFD + AY)\n");
    printf("  --plus3  ZX Spectrum +3 (paginación 0x7FFD+0x1FFD, FDC uPD765, discos)\n");

    printf("\nROMs esperadas:\n");
    printf("  zx48.rom     (16KB)\n");
    printf("  zx128.rom    (32KB)\n");
    printf("  plus3-0.rom  (16KB)\n");
    printf("  plus3-1.rom  (16KB)\n");
    printf("  plus3-2.rom  (16KB)\n");
    printf("  plus3-3.rom  (16KB)\n");

    printf("\nControles:\n");
    printf("  F1  -> iniciar cinta\n");
    printf("  F2  -> velocidad maxima / normal\n");
    printf("  F3  -> detener cinta\n");
    printf("  Cursores -> joystick Kempston, RAlt/RCtrl -> fire\n");
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    ZXModel model = ZX_MODEL_48K;

    // Lista de archivos (varios)
    const char* files[64];
    int file_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--128k") == 0) model = ZX_MODEL_128K;
        else if (strcmp(argv[i], "--48k") == 0) model = ZX_MODEL_48K;
        else if (strcmp(argv[i], "--plus3") == 0) model = ZX_MODEL_PLUS3;
        else if (argv[i][0] == '-' && argv[i][1] == '-') {
            print_usage(argv[0]);
            return 1;
        } else {
            if (file_count < (int)(sizeof(files)/sizeof(files[0])))
                files[file_count++] = argv[i];
        }
    }

    spectrum_init(&spec, model);

    // ROMs
    if (spectrum_load_rom48(&spec, "zx48.rom") != 0)
        printf("Aviso: 'zx48.rom' no encontrada.\n");

    if (model == ZX_MODEL_128K || model == ZX_MODEL_PLUS3) {
        if (spectrum_load_rom128(&spec, "zx128.rom") != 0)
            printf("Aviso: 'zx128.rom' no encontrada (32KB).\n");
    }

    if (model == ZX_MODEL_PLUS3) {
        if (spectrum_load_rom_plus3_set(&spec, "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom") != 0) {
            printf("Aviso: ROMs +3 no encontradas (plus3-0..3.rom). +3 arrancará en modo degradado.\n");
        }
        // Estado inicial +3
        spectrum_apply_1ffd_force(&spec, 0x00);
        spectrum_apply_7ffd_force(&spec, 0x00);
    }

    // Montaje de discos DSK: A luego B
    int drive_to_mount = 0;

    for (int i = 0; i < file_count; i++) {
        const char* file = files[i];

        if (ext_eq(file, ".tap")) {
            if (spectrum_load_tap(&spec, file) == 0)
                printf("TAP cargado. Escribe LOAD \"\" y pulsa F1 para iniciar.\n");
            else
                fprintf(stderr, "Error cargando TAP: %s\n", file);
        } else if (ext_eq(file, ".tzx")) {
            if (spectrum_load_tzx(&spec, file) == 0)
                printf("TZX cargado. Escribe LOAD \"\" y pulsa F1 para iniciar.\n");
            else
                fprintf(stderr, "Error cargando TZX: %s\n", file);
        } else if (ext_eq(file, ".sna")) {
            if (spectrum_load_sna(&spec, file) == 0)
                printf("Snapshot SNA cargado: %s\n", file);
            else
                fprintf(stderr, "Error cargando SNA: %s\n", file);
        } else if (ext_eq(file, ".dsk")) {
            if (model != ZX_MODEL_PLUS3) {
                printf("[DSK] Ignorado: sólo disponible en --plus3\n");
                continue;
            }
            int d = (drive_to_mount < 2) ? drive_to_mount : 0;
            if (fdc_load_dsk(&spec.fdc, d, file))
                printf("[DSK] Disco montado en drive %c: %s\n", d ? 'B' : 'A', file);
            else
                printf("[DSK] ERROR montando DSK en drive %c: %s\n", d ? 'B' : 'A', file);
            drive_to_mount++;
        } else {
            printf("Archivo no reconocido: %s\n", file);
        }
    }

    if (file_count == 0)
        print_usage(argv[0]);

    const uint32_t FRAME_MS = 1000 / 50;

    while (!spec.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) spec.quit = true;
            else if (e.type == SDL_KEYDOWN) spectrum_handle_key(&spec, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP) spectrum_handle_key(&spec, e.key.keysym.scancode, false);
        }

        spectrum_run_frame(&spec);

        if (spec.turbo_mode) {
            if ((spec.frame_counter & 7) == 0)
                spectrum_render(&spec);
        } else {
            spectrum_render(&spec);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS) SDL_Delay(FRAME_MS - elapsed);
        }
    }

    spectrum_destroy(&spec);
    return 0;
}
