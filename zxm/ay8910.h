#ifndef AY8910_H
#define AY8910_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Emulación mínima AY-3-8912/AY-3-8910 (PSG) suficiente para Spectrum 128/+2/+3.
// - 16 registros (0..15)
// - 3 canales de tono + generador de ruido
// - envolvente
// - reloj interno derivado del reloj de CPU (t-states)

typedef struct {
    // Registros PSG
    uint8_t regs[16];
    uint8_t sel;

    // Divisores internos (en ticks del reloj AY)
    uint16_t div16;   // acumula ticks para /16
    uint16_t div256;  // acumula ticks para /256

    // Tonos
    uint16_t tone_period[3];
    uint16_t tone_count[3];
    uint8_t  tone_out[3];

    // Ruido
    uint8_t  noise_period;
    uint16_t noise_count;
    uint32_t lfsr;       // 17-bit LFSR
    uint8_t  noise_out;

    // Envolvente
    uint16_t env_period;
    uint16_t env_count;
    uint8_t  env_shape;
    int8_t   env_step;
    uint8_t  env_vol;
    uint8_t  env_hold;
    uint8_t  env_alt;
    uint8_t  env_attack;
    uint8_t  env_continue;

    // Conversión desde t-states (CPU) a ticks AY
    uint32_t tick_accum; // acumulado en unidades de cpu_hz (resto)
    uint32_t cpu_hz;
    uint32_t ay_hz;

    // Último sample (por si quieres hacer smoothing externo)
    float last_sample;
} AY8910;

// Inicializa / resetea el chip y fija relojes.
void ay8910_reset(AY8910* a, uint32_t cpu_hz, uint32_t ay_hz);

// Selecciona registro (equivalente a OUT (FFFD),A en Spectrum 128).
static inline void ay8910_select(AY8910* a, uint8_t value) {
    a->sel = value & 0x0F;
}

// Lee el registro seleccionado (equivalente a IN A,(FFFD)).
uint8_t ay8910_read(const AY8910* a);

// Escribe en el registro seleccionado (equivalente a OUT (BFFD),A).
void ay8910_write_selected(AY8910* a, uint8_t value);

// Escribe un registro explícito (útil para tests).
void ay8910_write_reg(AY8910* a, uint8_t reg, uint8_t value);

// Avanza el estado interno usando t-states de CPU.
void ay8910_step_tstates(AY8910* a, uint32_t tstates);

// Mezcla salida mono normalizada (aprox. 0..1).
float ay8910_mix(AY8910* a);

#ifdef __cplusplus
}
#endif

#endif // AY8910_H
