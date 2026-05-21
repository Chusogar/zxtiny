#include "ay8910.h"

#include <string.h>

// Tabla de volumen AY (aprox. logarítmica)
static const float ay_vol[16] = {
    0.0f, 0.004f, 0.006f, 0.009f,
    0.013f, 0.019f, 0.028f, 0.041f,
    0.060f, 0.088f, 0.129f, 0.189f,
    0.276f, 0.403f, 0.587f, 0.857f
};

static void ay_env_restart(AY8910* a, uint8_t shape) {
    a->env_shape    = shape;
    a->env_continue = (shape >> 3) & 1;
    a->env_attack   = (shape >> 2) & 1;
    a->env_alt      = (shape >> 1) & 1;
    a->env_hold     = (shape >> 0) & 1;

    a->env_step = a->env_attack ? 1 : -1;
    a->env_vol  = a->env_attack ? 0 : 15;
    a->env_count = a->env_period ? a->env_period : 1;
}

static inline void ay_step_envelope(AY8910* a) {
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

static void ay_step_ticks(AY8910* a, uint32_t ticks) {
    for (uint32_t t = 0; t < ticks; t++) {
        a->div16++;
        if (a->div16 >= 16) {
            a->div16 = 0;

            // Tono
            for (int ch = 0; ch < 3; ch++) {
                if (a->tone_count[ch] == 0) a->tone_count[ch] = a->tone_period[ch];
                a->tone_count[ch]--;
                if (a->tone_count[ch] == 0) {
                    a->tone_out[ch] ^= 1;
                    a->tone_count[ch] = a->tone_period[ch];
                }
            }

            // Ruido
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

void ay8910_reset(AY8910* a, uint32_t cpu_hz, uint32_t ay_hz) {
    memset(a, 0, sizeof(*a));
    a->cpu_hz = cpu_hz ? cpu_hz : 3500000;
    a->ay_hz  = ay_hz  ? ay_hz  : 1773400;

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

uint8_t ay8910_read(const AY8910* a) {
    return a->regs[a->sel & 0x0F];
}

void ay8910_write_reg(AY8910* a, uint8_t reg, uint8_t value) {
    reg &= 0x0F;

    static const uint8_t masks[16] = {
        0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF
    };

    value &= masks[reg];
    a->regs[reg] = value;

    // Recalcular periodos
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

    if (reg == 13) ay_env_restart(a, value);
}

void ay8910_write_selected(AY8910* a, uint8_t value) {
    ay8910_write_reg(a, a->sel, value);
}

void ay8910_step_tstates(AY8910* a, uint32_t tstates) {
    // Convertir tstates (CPU) a ticks AY
    // ticks = tstates * ay_hz / cpu_hz
    uint64_t add = (uint64_t)tstates * (uint64_t)a->ay_hz;
    uint64_t acc = (uint64_t)a->tick_accum + add;
    uint32_t ticks = (uint32_t)(acc / a->cpu_hz);
    a->tick_accum = (uint32_t)(acc % a->cpu_hz);

    if (ticks) ay_step_ticks(a, ticks);
}

float ay8910_mix(AY8910* a) {
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

    float out = (sum / 3.0f) * 0.9f;
    a->last_sample = out;
    return out;
}
