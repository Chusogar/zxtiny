/*
 * psg.c - AY-3-8912 PSG implementation
 */

#include "psg.h"
#include <string.h>
#include <stdlib.h>

/* AY volume table (logarithmic, 16 levels) */
static const int16_t vol_table[16] = {
    0, 170, 240, 340, 480, 680, 960, 1360,
    1920, 2720, 3840, 5440, 7680, 10880, 15360, 21760
};

void psg_init(psg_t *psg)
{
    memset(psg, 0, sizeof(*psg));
    psg->noise_lfsr  = 1;
    psg->noise_out   = 1;
    psg->env_dir     = 1;
    psg->sample_rate = PSG_SAMPLE_RATE;
    /* All channels disabled by default */
    psg->reg[7] = 0xFF;
}

void psg_destroy(psg_t *psg)
{
    (void)psg;
}

static void psg_write_reg(psg_t *psg, uint8_t reg, uint8_t val)
{
    if (reg >= 16) return;
    psg->reg[reg] = val;

    switch (reg) {
    case 0: case 1: psg->tone_period[0] = (psg->reg[1] & 0xF) << 8 | psg->reg[0]; break;
    case 2: case 3: psg->tone_period[1] = (psg->reg[3] & 0xF) << 8 | psg->reg[2]; break;
    case 4: case 5: psg->tone_period[2] = (psg->reg[5] & 0xF) << 8 | psg->reg[4]; break;
    case 6:  psg->noise_period = val & 0x1F; break;
    case 11: case 12:
        psg->env_period = (psg->reg[12] << 8) | psg->reg[11];
        break;
    case 13: /* Envelope shape */
        psg->env_shape   = val & 0xF;
        psg->env_counter = 0;
        psg->env_step    = 0;
        psg->env_hold    = 0;
        /* Determine initial direction and alternate */
        if (val & 4) {
            psg->env_dir  = 1;  /* count up */
            psg->env_vol  = 0;
        } else {
            psg->env_dir  = -1; /* count down */
            psg->env_vol  = 15;
        }
        psg->env_alt = (val & 2) ? 1 : 0;
        break;
    }
}

static uint8_t psg_read_reg(psg_t *psg, uint8_t reg)
{
    if (reg >= 16) return 0xFF;
    return psg->reg[reg];
}

void psg_control(psg_t *psg, uint8_t bdir, uint8_t bc1, uint8_t data)
{
    /* PSG bus control:
     * BDIR=0, BC1=0: inactive
     * BDIR=0, BC1=1: read from PSG
     * BDIR=1, BC1=0: write data to PSG register
     * BDIR=1, BC1=1: latch address (register select) */
    if (bdir && bc1) {
        /* Latch address */
        psg->addr = data & 0x0F;
    } else if (bdir && !bc1) {
        /* Write to register */
        psg_write_reg(psg, psg->addr, data);
    }
    /* read handled via psg_read_data() */
    psg->bdir = bdir;
    psg->bc1  = bc1;
}

uint8_t psg_read_data(psg_t *psg)
{
    if (!psg->bdir && psg->bc1) {
        return psg_read_reg(psg, psg->addr);
    }
    return 0xFF;
}

void psg_write_data(psg_t *psg, uint8_t data)
{
    /* Called when Port A is written - stores in latch
     * Actual write happens on psg_control() with BDIR=1,BC1=0 */
    (void)psg;
    (void)data;
}

/*
 * Tick PSG for t_states T-states (at 4MHz Z80).
 * PSG clock = 1MHz = Z80/4
 * We generate audio samples at PSG_SAMPLE_RATE.
 */
void psg_tick(psg_t *psg, int t_states)
{
    /* PSG runs at 1MHz: 1 PSG tick per 4 Z80 T-states */
    int psg_ticks = t_states / 4;

    uint32_t clk_per_sample = PSG_CLOCK_HZ / PSG_SAMPLE_RATE;

    for (int t = 0; t < psg_ticks; t++) {
        /* ── Tone generators ────────────────────────────────────── */
        for (int ch = 0; ch < 3; ch++) {
            if (psg->tone_period[ch] > 0) {
                psg->tone_counter[ch]++;
                if (psg->tone_counter[ch] >= psg->tone_period[ch]) {
                    psg->tone_counter[ch] = 0;
                    psg->tone_out[ch] ^= 1;
                }
            } else {
                psg->tone_out[ch] = 1;
            }
        }

        /* ── Noise generator (17-bit LFSR) ─────────────────────── */
        psg->noise_counter++;
        if (psg->noise_counter >= (psg->noise_period ? psg->noise_period : 1)) {
            psg->noise_counter = 0;
            uint32_t bit = ((psg->noise_lfsr >> 16) ^ (psg->noise_lfsr >> 13)) & 1;
            psg->noise_lfsr = ((psg->noise_lfsr << 1) | bit) & 0x1FFFF;
            psg->noise_out  = psg->noise_lfsr & 1;
        }

        /* ── Envelope generator ─────────────────────────────────── */
        psg->env_counter++;
        if (psg->env_counter >= (psg->env_period ? psg->env_period * 8 : 8)) {
            psg->env_counter = 0;
            if (!psg->env_hold) {
                psg->env_vol += psg->env_dir;
                if (psg->env_vol > 15 || psg->env_vol < 0) {
                    /* End of envelope cycle */
                    uint8_t shape = psg->env_shape;
                    if (shape & 1) {
                        /* Hold */
                        psg->env_hold = 1;
                        psg->env_vol  = (psg->env_dir > 0) ? 15 : 0;
                    } else if (psg->env_alt) {
                        /* Alternate direction */
                        psg->env_dir  = -psg->env_dir;
                        psg->env_vol  = (psg->env_dir > 0) ? 0 : 15;
                    } else {
                        /* Restart */
                        psg->env_vol  = (psg->env_dir > 0) ? 0 : 15;
                    }
                }
            }
        }

        /* ── Sample output ──────────────────────────────────────── */
        psg->sample_frac += PSG_SAMPLE_RATE;
        if (psg->sample_frac >= PSG_CLOCK_HZ) {
            psg->sample_frac -= PSG_CLOCK_HZ;

            uint8_t mix = psg->reg[7];
            int16_t left = 0, right = 0;

            for (int ch = 0; ch < 3; ch++) {
                /* Tone enable: bit ch of reg7 (0=enabled) */
                int tone_en  = !((mix >> ch) & 1);
                int noise_en = !((mix >> (ch+3)) & 1);

                int output = (tone_en  && psg->tone_out[ch]) ||
                             (noise_en && psg->noise_out);

                if (output) {
                    /* Volume: bit4=use envelope */
                    uint8_t vol_reg = psg->reg[8 + ch];
                    int16_t vol;
                    if (vol_reg & 0x10) {
                        vol = vol_table[psg->env_vol & 0xF];
                    } else {
                        vol = vol_table[vol_reg & 0xF];
                    }
                    /* CPC stereo: A=left+center, B=center, C=right+center */
                    if (ch == 0)      { left  += vol; right += vol/2; }
                    else if (ch == 1) { left  += vol/2; right += vol/2; }
                    else              { left  += vol/2; right += vol; }
                }
            }

            /* Clamp */
            if (left  >  32767) left  =  32767;
            if (left  < -32768) left  = -32768;
            if (right >  32767) right =  32767;
            if (right < -32768) right = -32768;

            /* Write to ring buffer */
            int next = (psg->buf_write + 2) % (PSG_BUFFER_SIZE * 2);
            if (next != psg->buf_read) {
                psg->buffer[psg->buf_write]     = left;
                psg->buffer[psg->buf_write + 1] = right;
                psg->buf_write = next;
            }
        }
    }
}

int psg_fill_buffer(psg_t *psg, int16_t *out, int samples)
{
    int filled = 0;
    while (filled < samples && psg->buf_read != psg->buf_write) {
        out[filled*2]   = psg->buffer[psg->buf_read];
        out[filled*2+1] = psg->buffer[psg->buf_read+1];
        psg->buf_read = (psg->buf_read + 2) % (PSG_BUFFER_SIZE * 2);
        filled++;
    }
    /* Silence if underrun */
    while (filled < samples) {
        out[filled*2]   = 0;
        out[filled*2+1] = 0;
        filled++;
    }
    return filled;
}
