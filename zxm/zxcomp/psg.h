/*
 * psg.h - General Instrument AY-3-8912 PSG
 *
 * 3 tone channels (A/B/C), 1 noise channel, envelope generator.
 * CPC6128 uses this for all audio output.
 */

#ifndef PSG_H
#define PSG_H

#include <stdint.h>

#define PSG_SAMPLE_RATE   44100
#define PSG_CLOCK_HZ      1000000  /* 1 MHz (CPC: 4MHz/4) */
#define PSG_BUFFER_SIZE   4096

typedef struct psg_s {
    uint8_t  reg[16];          /* 16 registers */
    uint8_t  addr;             /* selected register */

    /* Tone generators */
    uint16_t tone_period[3];   /* tone period for A/B/C */
    uint16_t tone_counter[3];  /* down counters */
    uint8_t  tone_out[3];      /* current output 0/1 */

    /* Noise generator */
    uint16_t noise_period;
    uint16_t noise_counter;
    uint32_t noise_lfsr;       /* 17-bit LFSR */
    uint8_t  noise_out;

    /* Envelope generator */
    uint16_t env_period;
    uint32_t env_counter;
    uint8_t  env_shape;
    uint8_t  env_vol;
    int      env_step;
    int      env_hold;
    int      env_dir;
    int      env_alt;

    /* Output mixing */
    /* Audio ring buffer */
    int16_t  buffer[PSG_BUFFER_SIZE * 2];  /* stereo */
    int      buf_write;
    int      buf_read;

    /* Fractional sample counter */
    uint32_t sample_frac;
    uint32_t sample_rate;

    /* PSG state */
    uint8_t  bdir, bc1;
} psg_t;

void    psg_init       (psg_t *psg);
void    psg_destroy    (psg_t *psg);
void    psg_control    (psg_t *psg, uint8_t bdir, uint8_t bc1, uint8_t data);
uint8_t psg_read_data  (psg_t *psg);
void    psg_write_data (psg_t *psg, uint8_t data);
void    psg_tick       (psg_t *psg, int t_states);
int     psg_fill_buffer(psg_t *psg, int16_t *out, int samples);

#endif /* PSG_H */
