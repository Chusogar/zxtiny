#ifndef TZX_H
#define TZX_H

// =============================================================================
// tzx.h  –  Reproductor de ficheros TZX para emulador ZX Spectrum 48K
//
// El formato TZX (v1.20) define bloques con ID de 1 byte seguido de datos.
// Este módulo convierte cada bloque en una secuencia de pulsos expresada en
// T-states (3.5 MHz) que el emulador lee ciclo a ciclo a través de tzx_update().
//
// Bloques implementados
// ─────────────────────
//  0x10  Standard Speed Data          (equivale a un bloque TAP)
//  0x11  Turbo Speed Data             (tiempos de pilot/sync/bits configurables)
//  0x12  Pure Tone                    (N pulsos de duración fija)
//  0x13  Pulse Sequence               (hasta 255 pulsos de duración variable)
//  0x14  Pure Data                    (solo datos, sin pilot/sync)
//  0x15  Direct Recording             (muestras de 1 bit a frecuencia dada)
//  0x19  Generalized Data             (piloto+datos con alfabeto de símbolos)
//  0x20  Pause / Stop the Tape        (pausa en ms; 0 ms = stop)
//  0x21  Group Start                  (ignorado, solo metadato)
//  0x22  Group End                    (ignorado)
//  0x23  Jump To Block                (salto relativo)
//  0x24  Loop Start                   (inicio de bucle con contador)
//  0x25  Loop End                     (fin de bucle)
//  0x2A  Stop Tape if 48K             (detiene la cinta en modo 48K)
//  0x30  Text Description             (ignorado)
//  0x31  Message Block                (ignorado)
//  0x32  Archive Info                 (ignorado)
//  0x33  Hardware Type                (ignorado)
//  0x35  Custom Info                  (ignorado)
//  0x5A  Glue Block                   (ignorado)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Constantes de temporización estándar (T-states a 3.5 MHz)
// Iguales a las del TAP; se usan como valores por defecto en bloques 0x10.
// ---------------------------------------------------------------------------
#define TZX_STD_PILOT_PULSE    2168
#define TZX_STD_PILOT_HEADER   8063
#define TZX_STD_PILOT_DATA     3223
#define TZX_STD_SYNC1           667
#define TZX_STD_SYNC2           735
#define TZX_STD_BIT0            855
#define TZX_STD_BIT1           1710
#define TZX_STD_PAUSE_MS       1000   // ms → convertido a T-states en runtime

// Conversión ms → T-states (3 500 T-states por ms)
#define TZX_MS_TO_TSTATES(ms)  ((uint32_t)(ms) * 3500u)

// Máximo de pulsos en un bloque 0x13 (Pulse Sequence)
#define TZX_MAX_PULSES         255

// Máximo de niveles de bucle anidados (el spec solo define 1, pero lo dejamos a 8)
#define TZX_MAX_LOOP_DEPTH       8

// ---------------------------------------------------------------------------
// Estados internos de la máquina de pulsos
// ---------------------------------------------------------------------------
typedef enum {
    TZX_PS_IDLE = 0,      // Sin actividad
    TZX_PS_PILOT,         // Emitiendo tono piloto
    TZX_PS_SYNC1,         // Primer pulso de sync
    TZX_PS_SYNC2,         // Segundo pulso de sync
    TZX_PS_DATA,          // Emitiendo bits de datos
    TZX_PS_PURE_TONE,     // Bloque 0x12: pulsos puros
    TZX_PS_PULSE_SEQ,     // Bloque 0x13: secuencia de pulsos
    TZX_PS_PURE_DATA,     // Bloque 0x14: datos sin pilot
    TZX_PS_DIRECT,        // Bloque 0x15: grabación directa
    TZX_PS_GDB,           // Bloque 0x19: Generalized Data
    TZX_PS_PAUSE,         // Pausa entre bloques
} TZXPulseState;

// ---------------------------------------------------------------------------
// Contexto de bloque de datos activo
// Agrupa los parámetros variables que difieren entre bloques 0x10/0x11/0x14.
// ---------------------------------------------------------------------------
typedef struct {
    // Parámetros configurables del bloque actual
    uint16_t pilot_pulse;     // Duración del pulso piloto (T-states)
    uint16_t sync1_pulse;     // Duración del sync1
    uint16_t sync2_pulse;     // Duración del sync2
    uint16_t bit0_pulse;      // Duración semi-pulso bit 0
    uint16_t bit1_pulse;      // Duración semi-pulso bit 1
    uint16_t pilot_count;     // Número de pulsos piloto
    uint8_t  last_byte_bits;  // Bits usados en el último byte (1-8)
    uint32_t pause_ms;        // Pausa al final del bloque (ms)

    // Seguimiento de semi-pulsos dentro de cada bit
    uint8_t  half_pulse;      // 0 = primer semi-pulso, 1 = segundo semi-pulso

    // Posición dentro de los datos del bloque
    uint32_t data_offset;     // Offset en tzx_data donde empiezan los bytes
    uint32_t data_len;        // Longitud en bytes del bloque de datos
    uint32_t byte_pos;        // Byte actual
    uint8_t  bit_mask;        // Bit actual (0x80..0x01)
    uint8_t  bits_left;       // Bits restantes en el último byte
} TZXDataCtx;

// ---------------------------------------------------------------------------
// Contexto de Pure Tone (0x12) y Pulse Sequence (0x13)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t pulses[TZX_MAX_PULSES]; // Duraciones de pulsos (T-states)
    int      count;                  // Total de pulsos en la secuencia
    int      index;                  // Índice del pulso actual
} TZXPulseSeq;

// ---------------------------------------------------------------------------
// Contexto de Generalized Data Block (0x19)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t pause_ms;

    // Pilot/sync table parameters
    uint32_t totp;              // Total pilot/sync symbols (PRLE entries expand to this)
    uint8_t  npp;               // Max pulses per pilot symbol
    uint16_t asp;               // Pilot alphabet size (0→256 already resolved)
    uint32_t pilot_sym_off;     // Offset in data[] to pilot SYMDEF table
    uint32_t pilot_prle_off;    // Offset in data[] to pilot PRLE entries

    // Data table parameters
    uint32_t totd;              // Total data symbols
    uint8_t  npd;               // Max pulses per data symbol
    uint16_t asd;               // Data alphabet size (0→256 already resolved)
    uint32_t data_sym_off;      // Offset in data[] to data SYMDEF table
    uint32_t data_stream_off;   // Offset in data[] to data stream
    uint8_t  bits_per_sym;      // ceil(log2(ASD)), 0 if ASD<=1

    // Playback phase: 0=pilot, 1=data
    uint8_t  phase;

    // Pilot state
    uint32_t prle_idx;          // Current PRLE run index
    uint16_t cur_reps;          // Total repetitions for current run
    uint32_t sym_rep;           // Repetitions done in current run

    // Data state
    uint32_t data_sym_done;     // Data symbols fully output
    uint32_t data_bit_pos;      // Bit position in the data stream

    // Current symbol being output
    uint8_t  cur_sym;           // Symbol index in the alphabet
    uint8_t  pulse_idx;         // Current pulse within symbol
} TZXGDBCtx;

// ---------------------------------------------------------------------------
// Contexto de Direct Recording (0x15)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t tstates_per_sample; // T-states por muestra
    uint32_t data_offset;        // Offset en tzx_data
    uint32_t total_samples;      // Total de muestras
    uint32_t sample_idx;         // Muestra actual
    uint32_t sample_cycles;      // Ciclos restantes en la muestra actual
    uint8_t  last_byte_bits;     // Bits válidos en el último byte
    uint32_t pause_ms;
} TZXDirectCtx;

// ---------------------------------------------------------------------------
// Estructura principal del reproductor TZX
// ---------------------------------------------------------------------------
typedef struct {
    // ── Buffer de datos ───────────────────────────────────────────────────
    uint8_t* data;          // Contenido completo del fichero TZX
    uint32_t size;          // Tamaño en bytes

    // ── Tabla de bloques ─────────────────────────────────────────────────
    // Precalculada al cargar: offset de cada bloque dentro de data[].
    uint32_t* block_offsets; // [block_count]
    uint8_t*  block_ids;     // ID de cada bloque
    int       block_count;   // Total de bloques
    int       block_idx;     // Bloque actual

    // ── Máquina de estados de pulsos ─────────────────────────────────────
    TZXPulseState pulse_state;
    int32_t       pulse_cycles;  // T-states restantes del pulso actual (puede ser negativo)
    uint8_t       ear;           // Nivel EAR actual (0 ó 1)
    bool          active;        // true = cinta en marcha

    // ── Contextos de bloque ───────────────────────────────────────────────
    TZXDataCtx  dc;          // Datos variables del bloque de datos actual
    TZXPulseSeq ps;          // Secuencia de pulsos (0x12 / 0x13)
    TZXDirectCtx dr;         // Grabación directa (0x15)
    TZXGDBCtx   gdb;         // Generalized Data (0x19)

    // Pausa activa (bloque 0x20 o fin de bloque)
    uint32_t pause_cycles;   // T-states restantes de la pausa

    // ── Control de bucles (0x24 / 0x25) ──────────────────────────────────
    int loop_block[TZX_MAX_LOOP_DEPTH];   // Índice del bloque donde empezó el bucle
    int loop_count[TZX_MAX_LOOP_DEPTH];   // Repeticiones restantes
    int loop_depth;                       // Profundidad actual de anidamiento

    // ── Estadísticas ─────────────────────────────────────────────────────
    int blocks_played;
} TZXPlayer;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

// Carga un fichero TZX en el reproductor.
// Devuelve 0 en éxito, -1 en error.
int  tzx_load(TZXPlayer* tzx, const char* filename);

// Libera todos los recursos del reproductor.
void tzx_free(TZXPlayer* tzx);

// Inicia (o rebobina) la reproducción desde el principio.
void tzx_start(TZXPlayer* tzx);

// Avanza el reproductor 'cycles' T-states.
// Actualiza el campo 'ear' del reproductor; el llamador debe
// propagar tzx->ear al pin EAR del emulador (mic_bit).
void tzx_update(TZXPlayer* tzx, int cycles);

// Devuelve una cadena descriptiva del bloque actual (para debug/OSD).
const char* tzx_block_name(uint8_t id);

#endif // TZX_H