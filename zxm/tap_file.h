#ifndef TAP_FILE_H
#define TAP_FILE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Temporización TAP (T-states a ~3.5 MHz)
// ---------------------------------------------------------------------------
#define TAP_PILOT_PULSE   2168
#define TAP_PILOT_HEADER  8063
#define TAP_PILOT_DATA    3223
#define TAP_SYNC1_PULSE   667
#define TAP_SYNC2_PULSE   735
#define TAP_BIT0_PULSE    855
#define TAP_BIT1_PULSE    1710
#define TAP_PAUSE_CYCLES  3500000

// ---------------------------------------------------------------------------
// Máquina de estados TAP
// ---------------------------------------------------------------------------
typedef enum {
    TAP_STATE_IDLE = 0,
    TAP_STATE_PILOT,
    TAP_STATE_SYNC1,
    TAP_STATE_SYNC2,
    TAP_STATE_DATA,
    TAP_STATE_PAUSE
} TAPState;

// ---------------------------------------------------------------------------
// Reproductor TAP (por pulsos)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t pos;

    uint32_t block_len;
    uint32_t byte_pos;
    uint8_t  bit_mask;

    TAPState state;
    int      pilot_count;
    int32_t  pulse_cycles;

    uint8_t  ear;
    bool     active;
} TAPPlayer;

// Carga un archivo .tap en memoria y deja el player listo (no arranca).
// Devuelve 0 si OK, -1 si error.
int tap_file_load(TAPPlayer* t, const char* filename);

// Libera memoria asociada al TAP.
void tap_file_free(TAPPlayer* t);

// Inicia reproducción desde el principio (si hay datos cargados).
void tap_file_start(TAPPlayer* t);

// Avanza el reproductor 'cycles' t-states. Actualiza t->ear.
void tap_file_update(TAPPlayer* t, int cycles);

#ifdef __cplusplus
}
#endif

#endif // TAP_FILE_H
