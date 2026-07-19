#ifndef SN76489_H
#define SN76489_H

#include <stdint.h>

typedef struct SN76489 {
    uint32_t clock_hz;
    uint32_t sample_rate;
    uint64_t clock_accumulator;

    uint16_t tone_period[3];
    uint16_t tone_counter[3];
    uint8_t tone_output[3];
    uint8_t attenuation[4];

    uint8_t noise_control;
    uint16_t noise_counter;
    uint16_t noise_lfsr;
    uint8_t noise_output;
    uint8_t latched_register;
} SN76489;

void sn76489_init(SN76489 *chip, uint32_t clock_hz, uint32_t sample_rate);
void sn76489_reset(SN76489 *chip);
void sn76489_write(SN76489 *chip, uint8_t data);
int16_t sn76489_sample(SN76489 *chip);
void sn76489_generate(SN76489 *chip, int16_t *output, uint32_t samples);

#endif
