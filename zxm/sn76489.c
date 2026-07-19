/*
 * sn76489.c - Emulador sencillo del PSG Texas Instruments SN76489/SN76496.
 * Tres canales de tono, un canal de ruido y atenuación de 2 dB por paso.
 */
#include "sn76489.h"
#include <stddef.h>
#include <string.h>

/* Amplitud relativa, 2 dB por paso; 15 es silencio. */
static const int16_t volume_table[16] = {
    8191, 6506, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031, 819, 650, 516, 410, 326, 0
};

static uint16_t effective_tone_period(uint16_t period)
{
    period &= 0x03ff;
    return period == 0 ? 0x0400 : period;
}

static uint16_t noise_period(const SN76489 *chip)
{
    switch (chip->noise_control & 0x03) {
    case 0: return 0x10;
    case 1: return 0x20;
    case 2: return 0x40;
    default: return effective_tone_period(chip->tone_period[2]);
    }
}

static void reset_noise(SN76489 *chip)
{
    chip->noise_lfsr = 0x4000;
    chip->noise_output = 1;
    chip->noise_counter = noise_period(chip);
}

void sn76489_reset(SN76489 *chip)
{
    uint32_t clock_hz = chip->clock_hz;
    uint32_t sample_rate = chip->sample_rate;

    memset(chip, 0, sizeof(*chip));
    chip->clock_hz = clock_hz;
    chip->sample_rate = sample_rate;
    chip->latched_register = 0;

    for (int channel = 0; channel < 3; ++channel) {
        chip->tone_period[channel] = 0;
        chip->tone_counter[channel] = 0x0400;
        chip->tone_output[channel] = 1;
    }
    for (int channel = 0; channel < 4; ++channel)
        chip->attenuation[channel] = 0x0f;

    chip->noise_control = 0;
    reset_noise(chip);
}

void sn76489_init(SN76489 *chip, uint32_t clock_hz, uint32_t sample_rate)
{
    memset(chip, 0, sizeof(*chip));
    chip->clock_hz = clock_hz;
    chip->sample_rate = sample_rate ? sample_rate : 48000;
    sn76489_reset(chip);
}

void sn76489_write(SN76489 *chip, uint8_t data)
{
    uint8_t reg;

    if (data & 0x80) {
        reg = (data >> 4) & 0x07;
        chip->latched_register = reg;

        if (reg == 6) {
            chip->noise_control = data & 0x07;
            reset_noise(chip);
        } else if (reg & 1) {
            chip->attenuation[reg >> 1] = data & 0x0f;
        } else {
            int channel = reg >> 1;
            chip->tone_period[channel] =
                (chip->tone_period[channel] & 0x03f0) | (data & 0x0f);
        }
        return;
    }

    reg = chip->latched_register;
    if (reg <= 4 && !(reg & 1)) {
        int channel = reg >> 1;
        chip->tone_period[channel] =
            (chip->tone_period[channel] & 0x000f) |
            ((uint16_t)(data & 0x3f) << 4);
    } else if (reg == 6) {
        chip->noise_control = data & 0x07;
        reset_noise(chip);
    } else if (reg & 1) {
        chip->attenuation[reg >> 1] = data & 0x0f;
    }
}

static void clock_noise(SN76489 *chip)
{
    uint16_t feedback;

    if (chip->noise_control & 0x04)
        feedback = (chip->noise_lfsr ^ (chip->noise_lfsr >> 1)) & 1;
    else
        feedback = chip->noise_lfsr & 1;

    chip->noise_lfsr = (uint16_t)((chip->noise_lfsr >> 1) |
                                  (feedback << 14));
    chip->noise_output = chip->noise_lfsr & 1;
}

static void sn76489_tick(SN76489 *chip)
{
    for (int channel = 0; channel < 3; ++channel) {
        if (chip->tone_counter[channel] > 1) {
            --chip->tone_counter[channel];
        } else {
            chip->tone_counter[channel] =
                effective_tone_period(chip->tone_period[channel]);
            chip->tone_output[channel] ^= 1;
        }
    }

    if (chip->noise_counter > 1) {
        --chip->noise_counter;
    } else {
        chip->noise_counter = noise_period(chip);
        clock_noise(chip);
    }
}

int16_t sn76489_sample(SN76489 *chip)
{
    if (!chip || chip->sample_rate == 0)
        return 0;

    /* El contador interno del SN76489 trabaja a clock/16. */
    chip->clock_accumulator += chip->clock_hz;
    const uint64_t threshold = (uint64_t)chip->sample_rate * 16U;

    while (chip->clock_accumulator >= threshold) {
        chip->clock_accumulator -= threshold;
        sn76489_tick(chip);
    }

    int32_t mixed = 0;
    for (int channel = 0; channel < 3; ++channel) {
        int32_t amplitude = volume_table[chip->attenuation[channel] & 0x0f];
        mixed += chip->tone_output[channel] ? amplitude : -amplitude;
    }

    int32_t noise_amplitude = volume_table[chip->attenuation[3] & 0x0f];
    mixed += chip->noise_output ? noise_amplitude : -noise_amplitude;

    mixed /= 4;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    return (int16_t)mixed;
}

void sn76489_generate(SN76489 *chip, int16_t *output, uint32_t samples)
{
    if (!output)
        return;

    for (uint32_t i = 0; i < samples; ++i)
        output[i] = sn76489_sample(chip);
}
