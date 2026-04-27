#ifndef TZX_H
#define TZX_H

// =============================================================================
// tzx.h  –  Reproductor de ficheros TZX para emulador ZX Spectrum
//
// Gestión de bloques TZX adaptada al estilo de CPCEC (cpcec-k7.h):
//   - Fases de reproducción: heads / tones / datas / waves / tails
//   - Tabla de head codes (pilot/sync) en pares (count, pulse)
//   - Tabla de code items (definiciones de símbolos con polaridad y pulsos)
//   - Funciones helper: tzx10, tzx11, tzx12, tzx14, tzx19, tzx20
//
// Bloques implementados (mismos que CPCEC)
// ────────────────────────────────────────
//  0x10  NORMAL DATA           (bloque estándar)
//  0x11  CUSTOM DATA           (tiempos configurables)
//  0x12  PURE TONE             (tono puro)
//  0x13  PURE SYNC             (secuencia de pulsos)
//  0x14  PURE DATA             (solo datos)
//  0x15  SAMPLES               (grabación directa 1-bit)
//  0x19  GENERAL DATA          (datos generalizados con alfabeto)
//  0x20  HOLD                  (pausa / stop)
//  0x21  GROUP START            (metadato)
//  0x22  GROUP END              (metadato)
//  0x23  JUMP TO BLOCK          (salto relativo)
//  0x24  LOOP START             (inicio de bucle)
//  0x25  LOOP END               (fin de bucle)
//  0x26  CALL SEQUENCE          (secuencia de llamadas)
//  0x27  RETURN FROM CALL       (retorno de llamada)
//  0x28  SELECT BLOCK           (selección de bloque)
//  0x2A  STOP ON 48K            (stop condicional)
//  0x2B  SET SIGNAL LEVEL       (nivel de señal)
//  0x30  TEXT                   (metadato)
//  0x31  MESSAGE BLOCK          (metadato)
//  0x32  ARCHIVE INFO           (metadato)
//  0x33  HARDWARE TYPE          (metadato)
//  0x34  EMULATION INFO         (metadato)
//  0x35  CUSTOM INFO            (metadato)
//  0x40  SNAPSHOT INFO          (metadato)
//  0x5A  GLUE                   (metadato)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Estructura principal del reproductor TZX
//
// Los campos siguen la nomenclatura de CPCEC (tape_heads, tape_tones,
// tape_datas, tape_waves, tape_tails, tape_headcode[], tape_codeitem[], etc.)
// adaptados como miembros de un struct en lugar de variables globales.
// ---------------------------------------------------------------------------
typedef struct {
    // ── Buffer de datos ───────────────────────────────────────────────────
    uint8_t*  data;           // Contenido completo del fichero TZX
    uint32_t  size;           // Tamaño en bytes

    // ── Tabla de bloques (pre-indexada) ───────────────────────────────────
    uint32_t* block_offsets;  // Offset de cada bloque en data[]
    uint8_t*  block_ids;      // ID de cada bloque
    int       block_count;    // Total de bloques
    int       block_idx;      // Bloque actual (-1 antes de empezar)

    // ── Posición de lectura secuencial ────────────────────────────────────
    uint32_t  read_pos;       // Posición actual en data[] (equiv. tape_filetell)

    // ── Estado de reproducción ────────────────────────────────────────────
    uint8_t   ear;            // Nivel EAR actual (0 ó 1)
    bool      active;         // true = cinta en marcha

    // ── Head codes (pilot/sync) ──────────────────────────────────────────
    // Pares (loop_count, pulse_length), equiv. tape_headcode[] de CPCEC
    int       headcode[256 * 2];
    int       head;           // Índice actual en headcode[]
    int       heads;          // Entradas de head restantes

    // ── Code-item tables (definiciones de símbolos) ──────────────────────
    // Equiv. tape_codeitem[][]: [edge, pulse1, pulse2, ..., -1]
    int16_t   codeitem[256][32];
    int16_t*  code;           // Puntero actual dentro de codeitem
    int       item;           // Índice de pulso dentro del código

    // ── Contadores de fase ───────────────────────────────────────────────
    int       tones;          // Símbolos de tono codificados restantes
    int       datas;          // Símbolos/bits de datos restantes
    int       waves;          // Muestras de 1 bit restantes (block 0x15)
    int       tails;          // T-states restantes para cola/pausa

    // ── Estado de lectura de datos ───────────────────────────────────────
    int       mask;           // Máscara de bits para extraer símbolos
    int       bits;           // Bits por símbolo de datos
    int       byte_val;       // Byte actual en decodificación (equiv. tape_byte)
    int       time_count;     // Contador de repeticiones (equiv. tape_time)

    // ── Construcción de tablas de tonos/datos ─────────────────────────────
    int       tonecodes;      // Nº de símbolos de tono por construir (0 = ya hecho)
    int       toneitems;      // Máx pulsos por símbolo de tono
    int       datacodes;      // Nº de símbolos de datos por construir (0 = ya hecho)
    int       dataitems;      // Máx pulsos por símbolo de datos

    // ── Valores de configuración TZX ─────────────────────────────────────
    // Equiv. tape_tzx* de CPCEC
    int       tzxpilot;       // Duración del pulso piloto (T-states)
    int       tzxpilots;      // Número de pulsos piloto
    int       tzxsync1;       // Duración del pulso sync1
    int       tzxsync2;       // Duración del pulso sync2
    int       tzxbit0;        // Duración del semi-pulso bit 0
    int       tzxbit1;        // Duración del semi-pulso bit 1
    int       tzxhold;        // Pausa al final del bloque (ms)

    // ── Acumulador de temporización ──────────────────────────────────────
    int       n_acc;          // Acumulador de pulsos en T-states (equiv. tape_n)

    // ── Parámetros de wave (grabación directa, bloque 0x15) ──────────────
    int       wave_mask;      // T-states por muestra

    // ── Sub-estado de cola ───────────────────────────────────────────────
    int       tail;           // 0: borde inicial, 1: forzar nivel bajo

    // ── Control de bucles (bloques 0x24/0x25) ────────────────────────────
    int       loops;          // Contador de iteraciones de bucle
    int       loop_block;     // Índice del bloque Loop Start

    // ── Control de llamadas (bloques 0x26/0x27) ──────────────────────────
    int       calls;          // Llamadas restantes
    int       call_block;     // Índice del bloque Call Sequence
    uint32_t  call_table_pos; // Posición en data[] del siguiente offset de llamada

    // ── Soporte de aceleración de cinta ──────────────────────────────────
    int       feedable;       // ¿Se puede acelerar la cinta?

    // ── Estadísticas ─────────────────────────────────────────────────────
    int       blocks_played;
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

// Devuelve una cadena descriptiva del bloque (para debug/OSD).
// Usa la nomenclatura de CPCEC.
const char* tzx_block_name(uint8_t id);

#endif // TZX_H