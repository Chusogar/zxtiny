#include "cpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Instancia global del emulador
AmstradCPC cpc;

// -----------------------------------------------------------------------------
// Constantes CPC 6128
// -----------------------------------------------------------------------------
#define CPC_CPU_CLOCK_HZ   4000000u
#define AY_CLOCK_HZ        1000000u
#define AY_TICK_RATE_HZ    (AY_CLOCK_HZ / 16u)
#define DEFAULT_AUDIO_RATE 44100

// Paleta hardware del CPC a RGB (Hardware color 0-31)
static const uint32_t hw_palette[32] = {
    0xFF7F7F7F, 0xFF7F7F7F, 0xFF00FF7F, 0xFFFFFF7F, 0xFF00007F, 0xFFFF007F, 0xFF007F7F, 0xFFFF7F7F,
    0xFFFF007F, 0xFFFFFF7F, 0xFFFFFF00, 0xFFFFFFFF, 0xFFFF0000, 0xFFFF00FF, 0xFFFF7F00, 0xFFFF7FFF,
    0xFF00007F, 0xFF00FF7F, 0xFF00FF00, 0xFF00FFFF, 0xFF000000, 0xFF0000FF, 0xFF007F00, 0xFF007FFF,
    0xFF7F007F, 0xFF7FFF7F, 0xFF7FFF00, 0xFF7FFFFF, 0xFF7F0000, 0xFF7F00FF, 0xFF7F7F00, 0xFF7F7FFF
};

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline int16_t clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// -----------------------------------------------------------------------------
// AY volumen aproximado (tabla típica AY)
// -----------------------------------------------------------------------------
static const float ay_vol_table[16] = {
    0.0f,
    0.010f/1.866f, 0.014f/1.866f, 0.020f/1.866f, 0.029f/1.866f,
    0.043f/1.866f, 0.063f/1.866f, 0.093f/1.866f, 0.137f/1.866f,
    0.201f/1.866f, 0.294f/1.866f, 0.432f/1.866f, 0.635f/1.866f,
    0.931f/1.866f, 1.283f/1.866f, 1.866f/1.866f
};

// -----------------------------------------------------------------------------
// AY helpers
// -----------------------------------------------------------------------------
static void psg_recalc_periods(PSG* p) {
    for (int ch = 0; ch < 3; ch++) {
        uint16_t per = (uint16_t)p->registers[ch*2] |
                      ((uint16_t)(p->registers[ch*2 + 1] & 0x0F) << 8);
        if (per == 0) per = 1;
        p->tone_period[ch] = per;
        if (p->tone_count[ch] == 0 || p->tone_count[ch] > per) p->tone_count[ch] = per;
    }

    uint8_t np = p->registers[6] & 0x1F;
    if (np == 0) np = 1;
    p->noise_period = np;
    if (p->noise_count == 0 || p->noise_count > np) p->noise_count = np;

    uint16_t ep = (uint16_t)p->registers[11] | ((uint16_t)p->registers[12] << 8);
    if (ep == 0) ep = 1;
    p->env_period = ep;
    if (p->env_count == 0 || p->env_count > ep) p->env_count = ep;
}

static void psg_restart_envelope(PSG* p) {
    p->env_shape     = p->registers[13];
    p->env_continue  = (p->env_shape >> 3) & 1;
    p->env_attack    = (p->env_shape >> 2) & 1;
    p->env_alternate = (p->env_shape >> 1) & 1;
    p->env_hold      = (p->env_shape >> 0) & 1;

    p->env_holding = 0;
    p->env_step = p->env_attack ? 0 : 15;
    p->env_dir  = p->env_attack ? +1 : -1;

    if (p->env_period == 0) p->env_period = 1;
    p->env_count = p->env_period;
}

static void psg_reset(PSG* p) {
    memset(p, 0, sizeof(*p));
    p->noise_lfsr = 0x1FFFFu;
    p->noise_out  = 1;

    for (int i = 0; i < 3; i++) {
        p->tone_out[i] = 1;
        p->tone_period[i] = 1;
        p->tone_count[i]  = 1;
    }
    p->noise_period = 1;
    p->noise_count  = 1;
    p->env_period   = 1;
    p->env_count    = 1;

    psg_recalc_periods(p);
    psg_restart_envelope(p);
}

static void psg_write_reg(PSG* p, uint8_t reg, uint8_t val) {
    reg &= 0x0F;
    p->registers[reg] = val;

    switch (reg) {
        case 0: case 1: case 2: case 3: case 4: case 5:
        case 6:
        case 11: case 12:
            psg_recalc_periods(p);
            break;
        case 13:
            psg_recalc_periods(p);
            psg_restart_envelope(p);
            break;
        default:
            break;
    }
}

static void psg_tick(PSG* p, int ticks) {
    while (ticks-- > 0) {
        // Tono
        for (int ch = 0; ch < 3; ch++) {
            if (p->tone_count[ch] > 0) p->tone_count[ch]--;
            if (p->tone_count[ch] == 0) {
                p->tone_count[ch] = p->tone_period[ch];
                p->tone_out[ch] ^= 1;
            }
        }

        // Ruido (LFSR 17-bit: taps 0 y 3)
        if (p->noise_count > 0) p->noise_count--;
        if (p->noise_count == 0) {
            p->noise_count = p->noise_period;
            uint32_t bit0 = p->noise_lfsr & 1u;
            uint32_t bit3 = (p->noise_lfsr >> 3) & 1u;
            uint32_t fb = bit0 ^ bit3;
            p->noise_lfsr = (p->noise_lfsr >> 1) | (fb << 16);
            p->noise_out = (uint8_t)(p->noise_lfsr & 1u);
        }

        // Envolvente
        if (p->env_count > 0) p->env_count--;
        if (p->env_count == 0) {
            p->env_count = p->env_period;
            if (!p->env_holding) {
                p->env_step += p->env_dir;
                if (p->env_step < 0 || p->env_step > 15) {
                    if (!p->env_continue) {
                        p->env_step = 0;
                        p->env_holding = 1;
                    } else {
                        if (p->env_alternate) p->env_attack ^= 1;
                        if (p->env_hold) {
                            p->env_step = p->env_attack ? 15 : 0;
                            p->env_holding = 1;
                        } else {
                            p->env_step = p->env_attack ? 0 : 15;
                            p->env_dir  = p->env_attack ? +1 : -1;
                        }
                    }
                }
            }
        }
    }
}

static float psg_mix_sample(PSG* p) {
    uint8_t mixer = p->registers[7];
    float sum = 0.0f;

    for (int ch = 0; ch < 3; ch++) {
        int tone_enable  = ((mixer & (1 << ch)) == 0);
        int noise_enable = ((mixer & (1 << (3 + ch))) == 0);

        int t = tone_enable  ? p->tone_out[ch] : 1;
        int n = noise_enable ? p->noise_out     : 1;
        int on = t & n;

        uint8_t vreg = p->registers[8 + ch];
        int vol = (vreg & 0x10) ? (p->env_step & 15) : (vreg & 0x0F);
        float amp = on ? ay_vol_table[vol] : 0.0f;

        sum += amp;
    }

    return sum / 3.0f;
}

// -----------------------------------------------------------------------------
// Audio glue (SDL_QueueAudio)
// -----------------------------------------------------------------------------
static void audio_flush(AmstradCPC* hw) {
    if (!hw->audio_dev) { hw->audio_mixpos = 0; return; }
    if (hw->audio_mixpos <= 0) return;

    // Evita latencia acumulada (si SDL se queda atrás)
    uint32_t queued = SDL_GetQueuedAudioSize(hw->audio_dev);
    uint32_t max_q = (uint32_t)(hw->audio_rate * 2 * sizeof(int16_t)); // ~2s
    if (queued > max_q) SDL_ClearQueuedAudio(hw->audio_dev);

    SDL_QueueAudio(hw->audio_dev, hw->audio_mixbuf,
                   (uint32_t)(hw->audio_mixpos * (int)sizeof(int16_t)));
    hw->audio_mixpos = 0;
}

static void audio_push_sample(AmstradCPC* hw, int16_t s) {
    hw->audio_mixbuf[hw->audio_mixpos++] = s;
    if (hw->audio_mixpos >= (int)(sizeof(hw->audio_mixbuf)/sizeof(hw->audio_mixbuf[0]))) {
        audio_flush(hw);
    }
}

static void cpc_audio_step(AmstradCPC* hw, int cpu_cycles) {
    if (!hw->audio_dev) return;

    hw->audio_sample_accum += (uint32_t)cpu_cycles * (uint32_t)hw->audio_rate;

    while (hw->audio_sample_accum >= CPC_CPU_CLOCK_HZ) {
        hw->audio_sample_accum -= CPC_CPU_CLOCK_HZ;

        hw->ay_tick_accum += AY_TICK_RATE_HZ;
        int ay_ticks = (int)(hw->ay_tick_accum / (uint32_t)hw->audio_rate);
        hw->ay_tick_accum %= (uint32_t)hw->audio_rate;

        if (ay_ticks > 0) psg_tick(&hw->psg, ay_ticks);

        float s = psg_mix_sample(&hw->psg);
        int out = (int)(s * 32767.0f * 0.75f);
        audio_push_sample(hw, clamp16(out));
    }
}

// -----------------------------------------------------------------------------
// Gestión de memoria (paginación 128KB)
// -----------------------------------------------------------------------------
static uint8_t* get_read_ptr(AmstradCPC* hw, uint16_t addr) {
    uint8_t bank = (uint8_t)(addr >> 14);

    if (bank == 0 && hw->rom_lower_enabled) {
        return &hw->rom_lower[addr];
    }

    if (bank == 3 && hw->rom_upper_enabled) {
        uint8_t romsel = hw->selected_upper_rom & 0x3F;
        if (romsel == 7) return &hw->rom_amsdos[addr & 0x3FFF];
        return &hw->rom_upper[addr & 0x3FFF];
    }

    int ram_base_bank = bank;
    switch (hw->ram_bank_config & 0x07) {
        case 1: if (bank == 3) ram_base_bank = 7; break;
        case 2: if (bank == 3) ram_base_bank = 6; break;
        case 3: ram_base_bank = (bank == 3) ? 3 : (bank == 0 ? 0 : (bank == 1 ? 7 : 2)); break;
        case 4: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 4 : (bank == 2 ? 2 : 3)); break;
        case 5: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 5 : (bank == 2 ? 2 : 3)); break;
        case 6: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 6 : (bank == 2 ? 2 : 3)); break;
        case 7: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 7 : (bank == 2 ? 2 : 3)); break;
        default: break;
    }
    return &hw->ram[(ram_base_bank * 16384) + (addr & 0x3FFF)];
}

static uint8_t* get_write_ptr(AmstradCPC* hw, uint16_t addr) {
    uint8_t bank = (uint8_t)(addr >> 14);

    int ram_base_bank = bank;
    switch (hw->ram_bank_config & 0x07) {
        case 1: if (bank == 3) ram_base_bank = 7; break;
        case 2: if (bank == 3) ram_base_bank = 6; break;
        case 3: ram_base_bank = (bank == 3) ? 3 : (bank == 0 ? 0 : (bank == 1 ? 7 : 2)); break;
        case 4: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 4 : (bank == 2 ? 2 : 3)); break;
        case 5: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 5 : (bank == 2 ? 2 : 3)); break;
        case 6: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 6 : (bank == 2 ? 2 : 3)); break;
        case 7: ram_base_bank = (bank == 0) ? 0 : (bank == 1 ? 7 : (bank == 2 ? 2 : 3)); break;
        default: break;
    }
    return &hw->ram[(ram_base_bank * 16384) + (addr & 0x3FFF)];
}

static uint8_t mem_read(void* userdata, uint16_t addr) {
    return *get_read_ptr((AmstradCPC*)userdata, addr);
}
static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    *get_write_ptr((AmstradCPC*)userdata, addr) = val;
}

// -----------------------------------------------------------------------------
// Periféricos (I/O)
// -----------------------------------------------------------------------------
static uint8_t port_in(z80* z, uint16_t port) {
    AmstradCPC* hw = (AmstradCPC*)z->userdata;

    // ------------------------------------------------------------
// FDC µPD765A (CPC 6128 / DDI-1) - decodificación recomendable
// Puertos típicos:
//   &FA7E  (write) Motor On/Off flipflop
//   &FB7E  (read)  765 MSR (Main Status Register)
//   &FB7F  (r/w)   765 Data Register
// Importante: decodificación parcial (A10=0, A7=0, A8 y A0 seleccionan) [1](https://www.cpcwiki.eu/index.php?title=765_FDC&redirect=no&mobileaction=toggle_view_desktop)[3](https://www.cpcwiki.eu/index.php/765_FDC)
// ------------------------------------------------------------
{
    uint16_t p = port;

    // Acceso al 765: requiere A10=0 (0x0400), A7=0 (0x0080) y A8=1 (0x0100).
    // A0 selecciona MSR (0) o DATA (1). Los demás bits suelen ir a 1.
    if ( (p & 0x0480) == 0x0000 ) {          // A10=0 y A7=0
        if ( (p & 0x0100) == 0x0100 ) {      // A8=1 => 765 regs
            if ( (p & 0x0001) == 0 ) return fdc_read_status(&hw->fdc); // &FB7E
            else                    return fdc_read_data(&hw->fdc);   // &FB7F
        }
        // &FA7E (motor) no tiene lectura útil
    }
}

    // PPI
    if (!(port & 0x0800)) {
        if ((port & 0x0300) == 0x0100) { // Puerto B
            uint8_t vsync = (hw->cycles_in_frame >= 75000) ? 1 : 0;
            return (uint8_t)(0x1E | (vsync ? 0x01 : 0x00));
        }
        if ((port & 0x0300) == 0x0000) { // Puerto A (lectura PSG)
            if ((hw->psg.selected_reg & 0x0F) == 14) {
                uint8_t line = hw->ppi.port_c & 0x0F;
                return (line < 10) ? hw->keyboard_matrix[line] : 0xFF;
            }
            return hw->psg.registers[hw->psg.selected_reg & 0x0F];
        }
    }

    return 0xFF;
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    AmstradCPC* hw = (AmstradCPC*)z->userdata;

    // ------------------------------------------------------------
// FDC µPD765A - puertos CPC correctos [1](https://www.cpcwiki.eu/index.php?title=765_FDC&redirect=no&mobileaction=toggle_view_desktop)[2](https://cpcrulez.fr/codingBOOK_soft158a-ddi-1_firmware_0500.htm)
//   &FA7E (write) Motor On/Off (bit0)
//   &FB7F (write) Data register
//   &FB7E (write) no usado (MSR es read-only)
// Decodificación parcial: A10=0, A7=0, A8 selecciona 765, A0 selecciona MSR/DATA [1](https://www.cpcwiki.eu/index.php?title=765_FDC&redirect=no&mobileaction=toggle_view_desktop)[3](https://www.cpcwiki.eu/index.php/765_FDC)
// ------------------------------------------------------------
{
    uint16_t p = port;

    if ( (p & 0x0480) == 0x0000 ) {          // A10=0 y A7=0
        if ( (p & 0x0100) == 0x0000 ) {
            // A8=0 y típicamente &FA7E: motor flip-flop (solo escritura)
            // En CPC: escribir 0x00 apaga, 0x01 enciende (bit0) [1](https://www.cpcwiki.eu/index.php?title=765_FDC&redirect=no&mobileaction=toggle_view_desktop)[2](https://cpcrulez.fr/codingBOOK_soft158a-ddi-1_firmware_0500.htm)
            if ( (p & 0x00FF) == 0x7E ) {
                fdc_motor_control(&hw->fdc, val & 1);
                return;
            }
        } else {
            // A8=1 => registros 765
            if ( (p & 0x0001) == 1 ) {
                // &FB7F data
                fdc_write_data(&hw->fdc, val);
                return;
            } else {
                // &FB7E MSR write: no usado
                return;
            }
        }
    }
}

    // ROM select DFxx
    if ((port & 0xDF00) == 0xDF00) {
        hw->selected_upper_rom = val & 0x3F;
        return;
    }

    // Gate Array 7Fxx
    if (!(port & 0x8000) && (port & 0x4000)) {
        switch (val >> 6) {
            case 0:
                hw->pen_selected = (val & 0x10) ? 16 : (val & 0x0F);
                break;
            case 1:
                if (hw->pen_selected < 17) hw->palette[hw->pen_selected] = val & 0x1F;
                break;
            case 2:
                hw->screen_mode = val & 0x03;
                hw->rom_lower_enabled = (uint8_t)(!(val & 0x04));
                hw->rom_upper_enabled = (uint8_t)(!(val & 0x08));
                if (val & 0x10) hw->irq_counter = 0;
                break;
            case 3:
                hw->ram_bank_config = val;
                break;
        }
        return;
    }

    // CRTC BCxx/BDxx
    if (!(port & 0x4000) && (port & 0x8000)) {
        if (!(port & 0x0100)) hw->crtc.selected_reg = val & 0x1F;
        else if (hw->crtc.selected_reg < 18) hw->crtc.registers[hw->crtc.selected_reg] = val;
        return;
    }

    // PPI F4xx/F6xx
    if (!(port & 0x0800)) {
        if ((port & 0x0300) == 0x0200) {
            hw->ppi.port_c = val;

            // Simplificación BDIR/BC1 usando bits 7..6 de portC como ya hacías:
            // 11 => latch address (reg = portA[3:0])
            // 10 => write data   (data = portA)
            uint8_t ctrl = (uint8_t)(val >> 6);
            if (ctrl == 3) {
                hw->psg.selected_reg = hw->ppi.port_a & 0x0F;
            } else if (ctrl == 2) {
                psg_write_reg(&hw->psg, hw->psg.selected_reg & 0x0F, hw->ppi.port_a);
            }
        } else if ((port & 0x0300) == 0x0000) {
            hw->ppi.port_a = val;
        }
        return;
    }
}

// -----------------------------------------------------------------------------
// SNA v3 RLE (igual que tu base, con sintaxis limpia)
// -----------------------------------------------------------------------------
static int sna_rle_unpack_64k(uint8_t *out64k, const uint8_t *in, size_t inlen) {
    const uint8_t CTRL = 0xE5;
    size_t ip = 0, op = 0;

    while (op < 65536) {
        if (ip >= inlen) return -1;
        uint8_t b = in[ip++];

        if (b != CTRL) { out64k[op++] = b; continue; }

        if (ip >= inlen) return -2;
        uint8_t count = in[ip++];

        if (count == 0) { out64k[op++] = CTRL; continue; }

        if (ip >= inlen) return -3;
        uint8_t val = in[ip++];

        while (count-- && op < 65536) out64k[op++] = val;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Carga de SNA (tu lógica + resync PSG + limpiar cola audio)
// -----------------------------------------------------------------------------
int cpc_load_sna(AmstradCPC* hw, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t header[256];
    if (fread(header, 1, 256, f) != 256) { fclose(f); return -2; }
    if (memcmp(header, "MV - SNA", 8) != 0) { fclose(f); return -3; }

    uint8_t ver = header[0x10];
    if (ver < 1 || ver > 3) { fclose(f); return -4; }

    hw->cpu.f = header[0x11]; hw->cpu.a = header[0x12];
    hw->cpu.c = header[0x13]; hw->cpu.b = header[0x14];
    hw->cpu.e = header[0x15]; hw->cpu.d = header[0x16];
    hw->cpu.l = header[0x17]; hw->cpu.h = header[0x18];
    hw->cpu.r = header[0x19]; hw->cpu.i = header[0x1A];
    hw->cpu.iff1 = header[0x1B] & 1; hw->cpu.iff2 = header[0x1C] & 1;
    hw->cpu.ix = rd16(&header[0x1D]); hw->cpu.iy = rd16(&header[0x1F]);
    hw->cpu.sp = rd16(&header[0x21]); hw->cpu.pc = rd16(&header[0x23]);
    hw->cpu.interrupt_mode = header[0x25] & 3;

    hw->cpu.f_ = header[0x26]; hw->cpu.a_ = header[0x27];
    hw->cpu.c_ = header[0x28]; hw->cpu.b_ = header[0x29];
    hw->cpu.e_ = header[0x2A]; hw->cpu.d_ = header[0x2B];
    hw->cpu.l_ = header[0x2C]; hw->cpu.h_ = header[0x2D];

    hw->pen_selected = header[0x2E];
    for (int i = 0; i < 17; i++) hw->palette[i] = header[0x2F + i] & 0x1F;

    uint8_t ga_multi = header[0x40];
    uint8_t ram_cfg  = header[0x41];
    uint8_t crtc_sel = header[0x42];

    port_out(&hw->cpu, 0x7F00, (uint8_t)(0x80 | (ga_multi & 0x3F)));
    port_out(&hw->cpu, 0x7F00, (uint8_t)(0xC0 | (ram_cfg  & 0x3F)));

    hw->crtc.selected_reg = crtc_sel & 0x1F;
    for (int i = 0; i < 18; i++) hw->crtc.registers[i] = header[0x43 + i];

    hw->selected_upper_rom = header[0x55] & 0x3F;

    hw->ppi.port_a = header[0x56];
    hw->ppi.port_b = header[0x57];
    hw->ppi.port_c = header[0x58];
    hw->ppi.control= header[0x59];

    hw->psg.selected_reg = header[0x5A] & 0x0F;
    for (int i = 0; i < 16; i++) hw->psg.registers[i] = header[0x5B + i];

    uint16_t mem_kb = rd16(&header[0x6B]);
    memset(hw->ram, 0, sizeof(hw->ram));
    if (mem_kb != 0) {
        long remaining = fsize - 256;
        size_t dump_bytes = (size_t)mem_kb * 1024;
        if (dump_bytes > (size_t)remaining) dump_bytes = (size_t)remaining;
        if (dump_bytes > sizeof(hw->ram)) dump_bytes = sizeof(hw->ram);
        fread(hw->ram, 1, dump_bytes, f);
    }

    if (ver == 3) {
        while (1) {
            uint8_t chdr[8];
            size_t r = fread(chdr, 1, 8, f);
            if (r == 0) break;
            if (r != 8) break;

            char name[5];
            memcpy(name, chdr, 4);
            name[4] = 0;
            uint32_t clen = rd32(&chdr[4]);

            uint8_t* cbuf = (uint8_t*)malloc(clen);
            if (!cbuf) { fclose(f); return -5; }
            if (fread(cbuf, 1, clen, f) != clen) { free(cbuf); break; }

            if (name[0]=='M' && name[1]=='E' && name[2]=='M' && name[3]>='0' && name[3]<='8') {
                int bank = name[3] - '0';
                if (bank <= 1) {
                    uint8_t tmp[65536];
                    if (clen == 65536) memcpy(tmp, cbuf, 65536);
                    else {
                        if (sna_rle_unpack_64k(tmp, cbuf, clen) != 0) {
                            free(cbuf); fclose(f); return -6;
                        }
                    }
                    memcpy(&hw->ram[bank * 65536], tmp, 65536);
                }
            }
            free(cbuf);
        }
    }

    fclose(f);

    // Resync PSG tras snapshot
    psg_recalc_periods(&hw->psg);
    psg_restart_envelope(&hw->psg);

    // Limpia audio para evitar latigazo
    if (hw->audio_dev) SDL_ClearQueuedAudio(hw->audio_dev);
    hw->audio_sample_accum = 0;
    hw->ay_tick_accum = 0;
    hw->audio_mixpos = 0;

    hw->cycles_in_frame = 0;
    hw->irq_counter = 0;
    hw->cpu.halted = 0;
    hw->cpu.irq_pending = 0;

    printf("SNA OK: %s ver=%u ROMsel=%u PC=%04X SP=%04X mem=%uKB\n",
           filename, ver, hw->selected_upper_rom & 0x3F, hw->cpu.pc, hw->cpu.sp, mem_kb);
    return 0;
}

/* ============================================================
   TECLADO COMPLETO (tu mapeo, con fix en key-up)
   ============================================================ */

void cpc_handle_key(AmstradCPC* hw, SDL_Scancode key, bool pressed) {
    int row = -1, bit = -1;
    switch(key) {
        // Fila 0
        case SDL_SCANCODE_UP:        row=0; bit=0; break;
        case SDL_SCANCODE_RIGHT:     row=0; bit=1; break;
        case SDL_SCANCODE_DOWN:      row=0; bit=2; break;
        case SDL_SCANCODE_KP_9:      row=0; bit=3; break;
        case SDL_SCANCODE_KP_6:      row=0; bit=4; break;
        case SDL_SCANCODE_KP_3:      row=0; bit=5; break;
        case SDL_SCANCODE_KP_ENTER:  row=0; bit=6; break;
        case SDL_SCANCODE_KP_PERIOD: row=0; bit=7; break;

        // Fila 1
        case SDL_SCANCODE_LEFT:      row=1; bit=0; break;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:      row=1; bit=1; break; // Tecla COPY
        case SDL_SCANCODE_KP_7:      row=1; bit=2; break;
        case SDL_SCANCODE_KP_8:      row=1; bit=3; break;
        case SDL_SCANCODE_KP_5:      row=1; bit=4; break;
        case SDL_SCANCODE_KP_1:      row=1; bit=5; break;
        case SDL_SCANCODE_KP_2:      row=1; bit=6; break;
        case SDL_SCANCODE_KP_0:      row=1; bit=7; break;

        // Fila 2
        case SDL_SCANCODE_HOME:
        case SDL_SCANCODE_INSERT:    row=2; bit=0; break; // Tecla CLR
        case SDL_SCANCODE_RIGHTBRACKET: row=2; bit=1; break; // [ (Físico: Tecla a la derecha de P)
        case SDL_SCANCODE_RETURN:    row=2; bit=2; break;
        case SDL_SCANCODE_BACKSLASH: row=2; bit=3; break; // ] (Físico: Tecla encima de Intro)
        case SDL_SCANCODE_KP_4:      row=2; bit=4; break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:    row=2; bit=5; break;
        case SDL_SCANCODE_GRAVE:     row=2; bit=6; break; // \ (Físico: Tecla a la izquierda del 1)
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:     row=2; bit=7; break;

        // Fila 3
        case SDL_SCANCODE_EQUALS:    row=3; bit=0; break; // ^
        case SDL_SCANCODE_MINUS:     row=3; bit=1; break; // -
        case SDL_SCANCODE_LEFTBRACKET: row=3; bit=2; break; // @
        case SDL_SCANCODE_P:         row=3; bit=3; break;
        case SDL_SCANCODE_SEMICOLON: row=3; bit=4; break; // ;
        case SDL_SCANCODE_APOSTROPHE: row=3; bit=5; break; // :
        case SDL_SCANCODE_SLASH:     row=3; bit=6; break; // /
        case SDL_SCANCODE_PERIOD:    row=3; bit=7; break; // .

        // Fila 4
        case SDL_SCANCODE_0:         row=4; bit=0; break;
        case SDL_SCANCODE_9:         row=4; bit=1; break;
        case SDL_SCANCODE_O:         row=4; bit=2; break;
        case SDL_SCANCODE_I:         row=4; bit=3; break;
        case SDL_SCANCODE_L:         row=4; bit=4; break;
        case SDL_SCANCODE_K:         row=4; bit=5; break;
        case SDL_SCANCODE_M:         row=4; bit=6; break;
        case SDL_SCANCODE_COMMA:     row=4; bit=7; break;

        // Fila 5
        case SDL_SCANCODE_8:         row=5; bit=0; break;
        case SDL_SCANCODE_7:         row=5; bit=1; break;
        case SDL_SCANCODE_U:         row=5; bit=2; break;
        case SDL_SCANCODE_Y:         row=5; bit=3; break;
        case SDL_SCANCODE_H:         row=5; bit=4; break;
        case SDL_SCANCODE_J:         row=5; bit=5; break;
        case SDL_SCANCODE_N:         row=5; bit=6; break;
        case SDL_SCANCODE_SPACE:     row=5; bit=7; break;

        // Fila 6
        case SDL_SCANCODE_6:         row=6; bit=0; break;
        case SDL_SCANCODE_5:         row=6; bit=1; break;
        case SDL_SCANCODE_R:         row=6; bit=2; break;
        case SDL_SCANCODE_T:         row=6; bit=3; break;
        case SDL_SCANCODE_G:         row=6; bit=4; break;
        case SDL_SCANCODE_F:         row=6; bit=5; break;
        case SDL_SCANCODE_B:         row=6; bit=6; break;
        case SDL_SCANCODE_V:         row=6; bit=7; break;

        // Fila 7
        case SDL_SCANCODE_4:         row=7; bit=0; break;
        case SDL_SCANCODE_3:         row=7; bit=1; break;
        case SDL_SCANCODE_E:         row=7; bit=2; break;
        case SDL_SCANCODE_W:         row=7; bit=3; break;
        case SDL_SCANCODE_S:         row=7; bit=4; break;
        case SDL_SCANCODE_D:         row=7; bit=5; break;
        case SDL_SCANCODE_C:         row=7; bit=6; break;
        case SDL_SCANCODE_X:         row=7; bit=7; break;

        // Fila 8
        case SDL_SCANCODE_1:         row=8; bit=0; break;
        case SDL_SCANCODE_2:         row=8; bit=1; break;
        case SDL_SCANCODE_ESCAPE:    row=8; bit=2; break; // ESCAPE de CPC
        case SDL_SCANCODE_Q:         row=8; bit=3; break;
        case SDL_SCANCODE_TAB:       row=8; bit=4; break;
        case SDL_SCANCODE_A:         row=8; bit=5; break;
        case SDL_SCANCODE_CAPSLOCK:  row=8; bit=6; break;
        case SDL_SCANCODE_Z:         row=8; bit=7; break;

        // Fila 9
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_DELETE:    row=9; bit=7; break; // DEL

        default: break;
    }

    if (row != -1 && bit != -1) {
        if (pressed) {
            hw->keyboard_matrix[row] &= (uint8_t)~(1u << bit); // 0=pulsado
        } else {
            hw->keyboard_matrix[row] |= (uint8_t)(1u << bit);  // 1=libre  (FIX)
        }
    }
}


/* ============================================================
   RENDERIZADO
   ============================================================ */

// ---- helpers de decodificación CPC (formatos de píxel) ----
// Mode 0: 2 píxeles por byte, 16 colores (4bpp), bits intercalados.
// Pixel0 bits: b7->bit0, b3->bit1, b5->bit2, b1->bit3
// Pixel1 bits: b6->bit0, b2->bit1, b4->bit2, b0->bit3
// (ver "Display pixel data format") [1](https://cpctech.cpcwiki.de/docs/graphics.html)
static inline uint8_t cpc_mode0_pix0(uint8_t v) {
    return (uint8_t)(((v >> 7) & 1) |
                     (((v >> 3) & 1) << 1) |
                     (((v >> 5) & 1) << 2) |
                     (((v >> 1) & 1) << 3));
}
static inline uint8_t cpc_mode0_pix1(uint8_t v) {
    return (uint8_t)(((v >> 6) & 1) |
                     (((v >> 2) & 1) << 1) |
                     (((v >> 4) & 1) << 2) |
                     (((v >> 0) & 1) << 3));
}

// Mode 1: 4 píxeles por byte, 4 colores (2bpp), intercalado.
// Pixel0: (b7 + b3<<1), Pixel1:(b6 + b2<<1), Pixel2:(b5 + b1<<1), Pixel3:(b4 + b0<<1) [1](https://cpctech.cpcwiki.de/docs/graphics.html)
static inline uint8_t cpc_mode1_pix(uint8_t v, int p) {
    switch (p) {
        default:
        case 0: return (uint8_t)(((v >> 7) & 1) | (((v >> 3) & 1) << 1));
        case 1: return (uint8_t)(((v >> 6) & 1) | (((v >> 2) & 1) << 1));
        case 2: return (uint8_t)(((v >> 5) & 1) | (((v >> 1) & 1) << 1));
        case 3: return (uint8_t)(((v >> 4) & 1) | (((v >> 0) & 1) << 1));
    }
}

// Mode 2: 8 píxeles por byte, 2 colores (1bpp), bits lineales b7..b0 [1](https://cpctech.cpcwiki.de/docs/graphics.html)
static inline uint8_t cpc_mode2_pix(uint8_t v, int p) {
    return (uint8_t)((v >> (7 - p)) & 1);
}

// Mode 3 (no oficial): 2 píxeles por byte, 4 colores (2bpp), 4 bits no usados.
// Normalmente se comporta como modo 1 pero solo pixel0 y pixel1, con píxel ancho (low-res). [1](https://cpctech.cpcwiki.de/docs/graphics.html)[4](https://retrocomputing.stackexchange.com/questions/1024/how-did-mode-3-on-the-amstrad-cpc-work)
static inline uint8_t cpc_mode3_pix0(uint8_t v) {
    return (uint8_t)(((v >> 7) & 1) | (((v >> 3) & 1) << 1));
}
static inline uint8_t cpc_mode3_pix1(uint8_t v) {
    return (uint8_t)(((v >> 6) & 1) | (((v >> 2) & 1) << 1));
}

void cpc_render(AmstradCPC* hw) {
    // Dimensiones del buffer de salida (estilo CPCEC)
    // 768×544 con bordes, zona activa 640×400 centrada
    const int BUF_W = 768;
    const int BUF_H = 544;
    const int X0 = 64;  // margen izquierdo: (768-640)/2
    const int Y0 = 72;  // margen superior:  (544-400)/2

    // Rellena con el color del borde
    uint32_t border = hw_palette[hw->palette[16] & 0x1F];
    for (int i = 0; i < BUF_W * BUF_H; i++) hw->screen_buffer[i] = border;

    // CRTC
    int width_chars  = hw->crtc.registers[1] ? hw->crtc.registers[1] : 40;
    int height_chars = hw->crtc.registers[6] ? hw->crtc.registers[6] : 25;

    uint16_t ma_base = (uint16_t)(((hw->crtc.registers[12] & 0x3F) << 8) | hw->crtc.registers[13]);
    uint16_t screen_start = (uint16_t)((ma_base & 0x3000) << 2);

    int mode = (int)(hw->screen_mode & 3);

        // Todos los modos producen 16 píxeles por carácter (= 8 por byte),
    // igual que CPCEC: 40 chars × 16 = 640 píxeles activos.
    // Cada scanline se duplica verticalmente (200 líneas → 400).
    for (int char_y = 0; char_y < height_chars; char_y++) {
        for (int scanline = 0; scanline < 8; scanline++) {
            int y = Y0 + (char_y * 16) + (scanline * 2);
            if (y + 1 >= BUF_H) break;

            uint16_t ma_row = (uint16_t)((ma_base & 0x03FF) + (char_y * width_chars));
            uint16_t line_addr = (uint16_t)(screen_start |
                                            ((ma_row & 0x03FF) << 1) |
                                            ((uint16_t)scanline << 11));

            for (int x = 0; x < width_chars; x++) {
                for (int b = 0; b < 2; b++) {
                    uint8_t v = hw->ram[(line_addr + (x * 2) + b) & 0xFFFF];
                    int px_x = X0 + (x * 16) + (b * 8);

                    if (mode == 0) {
                        uint8_t pen0 = cpc_mode0_pix0(v) & 0x0F;
                        uint8_t pen1 = cpc_mode0_pix1(v) & 0x0F;
                        uint32_t c0 = hw_palette[hw->palette[pen0] & 0x1F];
                        uint32_t c1 = hw_palette[hw->palette[pen1] & 0x1F];
                        hw->screen_buffer[y * BUF_W + px_x + 0] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 1] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 2] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 3] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 4] = c1;
                        hw->screen_buffer[y * BUF_W + px_x + 5] = c1;
                        hw->screen_buffer[y * BUF_W + px_x + 6] = c1;
                        hw->screen_buffer[y * BUF_W + px_x + 7] = c1;
                    }
                    else if (mode == 1) {
                        for (int p = 0; p < 4; p++) {
                            uint8_t pen = cpc_mode1_pix(v, p) & 0x03;
                            uint32_t c = hw_palette[hw->palette[pen] & 0x1F];
                            hw->screen_buffer[y * BUF_W + px_x + p * 2 + 0] = c;
                            hw->screen_buffer[y * BUF_W + px_x + p * 2 + 1] = c;
                        }
                    }
                    else if (mode == 2) {
                        for (int p = 0; p < 8; p++) {
                            uint8_t pen = cpc_mode2_pix(v, p) & 0x01;
                            hw->screen_buffer[y * BUF_W + px_x + p] =
                                hw_palette[hw->palette[pen] & 0x1F];
                        }
                    }
                    else {
                        uint8_t pen0 = cpc_mode3_pix0(v) & 0x03;
                        uint8_t pen1 = cpc_mode3_pix1(v) & 0x03;
                        uint32_t c0 = hw_palette[hw->palette[pen0] & 0x1F];
                        uint32_t c1 = hw_palette[hw->palette[pen1] & 0x1F];
                        hw->screen_buffer[y * BUF_W + px_x + 0] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 1] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 2] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 3] = c0;
                        hw->screen_buffer[y * BUF_W + px_x + 4] = c1;
                        hw->screen_buffer[y * BUF_W + px_x + 5] = c1;
                        hw->screen_buffer[y * BUF_W + px_x + 6] = c1;
                        hw->screen_buffer[y * BUF_W + px_x + 7] = c1;
                    }
                }
            }

            memcpy(&hw->screen_buffer[(y + 1) * BUF_W],
                   &hw->screen_buffer[y * BUF_W],
                   (size_t)BUF_W * sizeof(uint32_t));
        }
    }

    SDL_UpdateTexture(hw->texture, NULL, hw->screen_buffer, BUF_W * 4);
    SDL_RenderCopy(hw->renderer, hw->texture, NULL, NULL);
    SDL_RenderPresent(hw->renderer);
}

// -----------------------------------------------------------------------------
// Ejecución de frame (CPU + IRQ + FDC + AUDIO)
// -----------------------------------------------------------------------------
void cpc_run_frame(AmstradCPC* hw) {
    hw->cycles_in_frame = 0;

    while (hw->cycles_in_frame < 80000) {
        int c = (int)z80_step(&hw->cpu);
        hw->cycles_in_frame += c;
        hw->irq_counter += c;

        // AUDIO: genera muestras según ciclos
        cpc_audio_step(hw, c);

        // FDC
        fdc_tick(&hw->fdc, c);
        if (fdc_irq(&hw->fdc)) z80_pulse_irq(&hw->cpu, 0xFF);

        // IRQ CPC (~300Hz)
        if (hw->irq_counter >= 12800) {
            hw->irq_counter -= 12800;
            z80_pulse_irq(&hw->cpu, 0xFF);
        }
    }

    // Empuja audio pendiente del frame
    audio_flush(hw);
}

// -----------------------------------------------------------------------------
// Inicialización y ROMs
// -----------------------------------------------------------------------------
void cpc_init(AmstradCPC* hw) {
    memset(hw, 0, sizeof(AmstradCPC));
    for (int i = 0; i < 10; i++) hw->keyboard_matrix[i] = 0xFF;

    hw->rom_lower_enabled = 1;
    hw->rom_upper_enabled = 1;
    hw->selected_upper_rom = 0;
    hw->ram_bank_config = 0;

    // FDC
    fdc_init(&hw->fdc);
    hw->fdc.drives[0].ready = false;
    hw->fdc.drives[1].ready = false;

    // PSG
    psg_reset(&hw->psg);

    // CPU
    z80_init(&hw->cpu);
    hw->cpu.userdata = hw;
    hw->cpu.read_byte = mem_read;
    hw->cpu.write_byte = mem_write;
    hw->cpu.port_in = port_in;
    hw->cpu.port_out = port_out;
    z80_reset(&hw->cpu);

    // SDL (VIDEO + AUDIO)
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    hw->window = SDL_CreateWindow("Amstrad CPC 6128",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  768, 544, SDL_WINDOW_RESIZABLE);

    hw->renderer = SDL_CreateRenderer(hw->window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(hw->renderer, 768, 544);
    hw->texture = SDL_CreateTexture(hw->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, 768, 544);

    // Audio init (queue)
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = DEFAULT_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = NULL;

    hw->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!hw->audio_dev) {
        printf("SDL audio: no se pudo abrir dispositivo: %s\n", SDL_GetError());
        hw->audio_rate = DEFAULT_AUDIO_RATE;
    } else {
        hw->audio_rate = have.freq;
        SDL_PauseAudioDevice(hw->audio_dev, 0);
        printf("SDL audio: %d Hz mono S16\n", hw->audio_rate);
    }

    hw->audio_sample_accum = 0;
    hw->ay_tick_accum = 0;
    hw->audio_mixpos = 0;
}

int cpc_load_roms(AmstradCPC* hw, const char* firmware_basic_32k, const char* file_amsdos) {
    FILE* f = fopen(firmware_basic_32k, "rb");
    if (!f) { printf("Error: No se pudo abrir ROM: %s\n", firmware_basic_32k); return -1; }

    size_t read_lower = fread(hw->rom_lower, 1, 16384, f);
    size_t read_upper = fread(hw->rom_upper, 1, 16384, f);
    fclose(f);

    if (read_lower != 16384 || read_upper != 16384) {
        printf("Aviso: ROM 32KB no tiene tamaño esperado.\n");
        return -2;
    }
    printf("ROM 32KB (Firmware+Basic) cargada correctamente.\n");

    f = fopen(file_amsdos, "rb");
    if (!f) { printf("Error: No se pudo abrir ROM AMSDOS: %s\n", file_amsdos); return -3; }

    size_t read_amsdos = fread(hw->rom_amsdos, 1, 16384, f);
    fclose(f);

    if (read_amsdos != 16384) {
        printf("Aviso: ROM AMSDOS no tiene tamaño esperado.\n");
        return -4;
    }
    printf("ROM AMSDOS 16KB cargada correctamente.\n");
    return 0;
}

void cpc_destroy(AmstradCPC* hw) {
    if (hw->audio_dev) {
        SDL_ClearQueuedAudio(hw->audio_dev);
        SDL_CloseAudioDevice(hw->audio_dev);
        hw->audio_dev = 0;
    }
    if (hw->texture) SDL_DestroyTexture(hw->texture);
    if (hw->renderer) SDL_DestroyRenderer(hw->renderer);
    if (hw->window) SDL_DestroyWindow(hw->window);
    SDL_Quit();
}

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    cpc_init(&cpc);
    cpc_load_roms(&cpc, "cpc6128.rom", "amsdos.rom");

    int drive_to_mount = 0;

for (int i = 1; i < argc; i++) {
    const char* ext = strrchr(argv[i], '.');
    if (!ext) continue;

    if (!strcasecmp(ext, ".sna")) {
        cpc_load_sna(&cpc, argv[i]);
    } else if (!strcasecmp(ext, ".dsk")) {
        int d = (drive_to_mount < 2) ? drive_to_mount : 0; // si pasan más, sobreescribe A
        if (fdc_load_dsk(&cpc.fdc, d, argv[i]) == 0) {
            // si tu FDC no marca ready internamente, fuerza ready
            cpc.fdc.drives[d].ready = true;
            printf("Disco montado en drive %c: %s\n", d ? 'B' : 'A', argv[i]);
        } else {
            printf("ERROR montando DSK en drive %c: %s\n", d ? 'B' : 'A', argv[i]);
        }
        drive_to_mount++;
    }
}

    while (!cpc.quit) {
        uint32_t start = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) cpc.quit = true;
            else if (e.type == SDL_KEYDOWN) cpc_handle_key(&cpc, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP) cpc_handle_key(&cpc, e.key.keysym.scancode, false);
        }

        cpc_run_frame(&cpc);
        cpc_render(&cpc);

        uint32_t elapsed = SDL_GetTicks() - start;
        if (elapsed < 20) SDL_Delay(20 - elapsed);
    }

    cpc_destroy(&cpc);
    return 0;
}