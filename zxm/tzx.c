// =============================================================================
// tzx.c  –  Reproductor de ficheros TZX para emulador ZX Spectrum
//
// Referencia del formato: https://worldofspectrum.net/TZXformat.html
//   (especificación TZX v1.20)
//
// Gestión de bloques adaptada al estilo de CPCEC (cpcec-k7.h):
//   - Lectura secuencial del buffer con tzx_getc/getcc/getccc/getcccc
//   - Fases de reproducción: heads → tones → datas → waves → tails
//   - Helpers: tzx_tzx10, tzx_tzx11, tzx_tzx12, tzx_tzx14, tzx_tzx19, tzx_tzx20
//   - Procesamiento de bloques inline en el bucle principal
//
// Arquitectura general
// ────────────────────
// tzx_load()   → lee el fichero, verifica cabecera, construye la tabla de bloques.
// tzx_start()  → rebobina e inicia la reproducción.
// tzx_update() → llamada cada vez que el Z80 ejecuta instrucciones.
//                Consume 'cycles' T-states y avanza la máquina de fases.
//
// Flujo de reproducción (cada iteración del bucle principal)
// ─────────────────────────────────────────────────────────
//  heads  → pulsos piloto/sync (headcode[]: pares count/pulse)
//  tones  → símbolos codificados con tabla de codeitem (bloque 0x19 pilot)
//  datas  → bits/símbolos de datos con tabla de codeitem
//  waves  → muestras directas de 1 bit (bloque 0x15)
//  tails  → pausa al final del bloque
//  (else) → leer y procesar el siguiente bloque TZX
// =============================================================================

#include "tzx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Utilidades de lectura little-endian desde el buffer
// Estilo CPCEC: tape_getc, tape_getcc, tape_getccc, tape_getcccc
// ---------------------------------------------------------------------------
static inline int tzx_getc(TZXPlayer* t) {
    if (t->read_pos >= t->size) return -1;
    return t->data[t->read_pos++];
}
static inline int tzx_getcc(TZXPlayer* t) {
    int i; if ((i = tzx_getc(t)) >= 0) i |= tzx_getc(t) << 8; return i;
}
static inline int tzx_getccc(TZXPlayer* t) {
    int i; if ((i = tzx_getc(t)) >= 0) if ((i |= tzx_getc(t) << 8) >= 0) i |= tzx_getc(t) << 16; return i;
}
static inline int tzx_getcccc(TZXPlayer* t) {
    int i; if ((i = tzx_getc(t)) >= 0) if ((i |= tzx_getc(t) << 8) >= 0) if ((i |= tzx_getc(t) << 16) >= 0) i |= tzx_getc(t) << 24; return i;
}
static inline void tzx_skip(TZXPlayer* t, int n) {
    t->read_pos += (uint32_t)n;
    if (t->read_pos > t->size) t->read_pos = t->size;
}
static inline void tzx_undo(TZXPlayer* t) {
    if (t->read_pos > 0) --t->read_pos;
}

// ---------------------------------------------------------------------------
// Utilidades de lectura directa del buffer (para indexado pre-carga)
// ---------------------------------------------------------------------------
static inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd24(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// ---------------------------------------------------------------------------
// Nombres de bloque (nomenclatura CPCEC)
// ---------------------------------------------------------------------------
const char* tzx_block_name(uint8_t id) {
    switch (id) {
    case 0x10: return "NORMAL DATA";
    case 0x11: return "CUSTOM DATA";
    case 0x12: return "PURE TONE";
    case 0x13: return "PURE SYNC";
    case 0x14: return "PURE DATA";
    case 0x15: return "SAMPLES";
    case 0x19: return "GENERAL DATA";
    case 0x20: return "HOLD";
    case 0x21: return "GROUP START";
    case 0x22: return "GROUP END";
    case 0x23: return "JUMP TO BLOCK";
    case 0x24: return "LOOP START";
    case 0x25: return "LOOP END";
    case 0x26: return "CALL SEQUENCE";
    case 0x27: return "RETURN FROM CALL";
    case 0x28: return "SELECT BLOCK";
    case 0x2A: return "STOP ON 48K";
    case 0x2B: return "SET SIGNAL LEVEL";
    case 0x30: return "TEXT";
    case 0x31: return "MESSAGE BLOCK";
    case 0x32: return "ARCHIVE INFO";
    case 0x33: return "HARDWARE TYPE";
    case 0x34: return "EMULATION INFO";
    case 0x35: return "CUSTOM INFO";
    case 0x40: return "SNAPSHOT INFO";
    case 0x5A: return "GLUE";
    default:   return "UNKNOWN!";
    }
}

// =============================================================================
// Cálculo del tamaño del cuerpo de un bloque (equiv. tape_tzx1size de CPCEC)
//
// Devuelve el tamaño en bytes del cuerpo del bloque apuntado por 'off'
// (SIN incluir el byte de ID). Devuelve 0 si el bloque es desconocido
// o el buffer está incompleto.
// =============================================================================
static uint32_t tzx_block_body_size(const uint8_t* data, uint32_t size, uint32_t off) {
    if (off >= size) return 0;
    uint8_t id = data[off];
    const uint8_t* p = data + off + 1;
    uint32_t rem = size - off - 1;

    switch (id) {
    case 0x10: // NORMAL DATA: pause(2) + len(2) + data
        if (rem < 4) return 0;
        return 4 + rd16(p + 2);
    case 0x11: // CUSTOM DATA: header(18) + data
        if (rem < 18) return 0;
        return 18 + rd24(p + 15);
    case 0x12: // PURE TONE: pulse(2) + count(2)
        return 4;
    case 0x13: // PURE SYNC: count(1) + N*pulse(2)
        if (rem < 1) return 0;
        return 1 + (uint32_t)p[0] * 2;
    case 0x14: // PURE DATA: header(10) + data
        if (rem < 10) return 0;
        return 10 + rd24(p + 7);
    case 0x15: // SAMPLES: header(8) + data
        if (rem < 8) return 0;
        return 8 + rd24(p + 5);
    case 0x19: // GENERAL DATA: length(4) + body
        if (rem < 4) return 0;
        return 4 + rd32(p);
    case 0x20: // HOLD: pause(2)
        return 2;
    case 0x2A: // STOP ON 48K: reserved(4)
        return 4;
    case 0x21: // GROUP START: len(1) + name
        if (rem < 1) return 0;
        return 1 + p[0];
    case 0x22: // GROUP END
        return 0;
    case 0x23: // JUMP TO BLOCK: offset(2)
        return 2;
    case 0x24: // LOOP START: count(2)
        return 2;
    case 0x25: // LOOP END
        return 0;
    case 0x26: // CALL SEQUENCE: count(2) + N*offset(2)
        if (rem < 2) return 0;
        return 2 + rd16(p) * 2;
    case 0x27: // RETURN FROM CALL
        return 0;
    case 0x28: // SELECT BLOCK: length(2) + data
        if (rem < 2) return 0;
        return 2 + rd16(p);
    case 0x31: // MESSAGE BLOCK: time(1) + len(1) + text
        if (rem < 2) return 0;
        return 2 + p[1];
    case 0x30: // TEXT: len(1) + text
        if (rem < 1) return 0;
        return 1 + p[0];
    case 0x32: // ARCHIVE INFO: length(2) + data
        if (rem < 2) return 0;
        return 2 + rd16(p);
    case 0x33: // HARDWARE TYPE: count(1) + N*3
        if (rem < 1) return 0;
        return 1 + (uint32_t)p[0] * 3;
    case 0x34: // EMULATION INFO: 8 bytes fijos
        return 8;
    case 0x35: // CUSTOM INFO: id(16) + length(4) + data
        if (rem < 20) return 0;
        return 20 + rd32(p + 16);
    case 0x40: // SNAPSHOT INFO: type(1) + length(3) + data
        if (rem < 4) return 0;
        return 4 + (rd32(p) >> 8);
    case 0x5A: // GLUE: 9 bytes
        return 9;
    default:   // UNKNOWN: length(4) + data
        if (rem < 4) return 0;
        return 4 + rd32(p);
    }
}

// ---------------------------------------------------------------------------
// Construcción de la tabla de bloques
// ---------------------------------------------------------------------------
static int tzx_index_blocks(TZXPlayer* t) {
    int count = 0;
    uint32_t off = 10; // Cabecera TZX = 10 bytes
    while (off < t->size) {
        uint32_t body = tzx_block_body_size(t->data, t->size, off);
        count++;
        off += 1 + body;
    }
    if (count == 0) return -1;

    t->block_offsets = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    t->block_ids     = (uint8_t*) malloc((size_t)count * sizeof(uint8_t));
    if (!t->block_offsets || !t->block_ids) return -1;

    off = 10;
    for (int i = 0; i < count; i++) {
        t->block_offsets[i] = off;
        t->block_ids[i]     = t->data[off];
        uint32_t body = tzx_block_body_size(t->data, t->size, off);
        off += 1 + body;
    }
    t->block_count = count;
    return 0;
}

// =============================================================================
// Carga del fichero
// =============================================================================
int tzx_load(TZXPlayer* tzx, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) { perror("[TZX] fopen"); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 10) { fclose(f); fprintf(stderr, "[TZX] Fichero demasiado pequeño.\n"); return -1; }

    uint8_t hdr[10];
    fread(hdr, 1, 10, f);
    if (memcmp(hdr, "ZXTape!", 7) != 0 || hdr[7] != 0x1A) {
        fclose(f);
        fprintf(stderr, "[TZX] Cabecera inválida.\n");
        return -1;
    }
    printf("[TZX] Versión %d.%d\n", hdr[8], hdr[9]);

    tzx_free(tzx);
    tzx->data = (uint8_t*)malloc((size_t)sz);
    if (!tzx->data) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    fread(tzx->data, 1, (size_t)sz, f);
    fclose(f);
    tzx->size = (uint32_t)sz;

    if (tzx_index_blocks(tzx) != 0) {
        tzx_free(tzx);
        fprintf(stderr, "[TZX] Error indexando bloques.\n");
        return -1;
    }

    printf("[TZX] %d bloques encontrados en '%s'\n", tzx->block_count, filename);
    for (int i = 0; i < tzx->block_count; i++) {
        printf("[TZX]   Bloque %3d  ID=0x%02X  off=0x%05X  %s\n",
               i, tzx->block_ids[i], tzx->block_offsets[i],
               tzx_block_name(tzx->block_ids[i]));
    }

    tzx->active    = false;
    tzx->ear       = 0;
    tzx->block_idx = -1;
    tzx->heads = tzx->tones = tzx->datas = tzx->waves = tzx->tails = 0;
    tzx->loops = tzx->calls = 0;
    return 0;
}

void tzx_free(TZXPlayer* tzx) {
    free(tzx->data);          tzx->data          = NULL;
    free(tzx->block_offsets); tzx->block_offsets = NULL;
    free(tzx->block_ids);     tzx->block_ids     = NULL;
    tzx->size        = 0;
    tzx->block_count = 0;
    tzx->block_idx   = -1;
    tzx->active      = false;
    tzx->ear         = 0;
    tzx->heads = tzx->tones = tzx->datas = tzx->waves = tzx->tails = 0;
    tzx->loops = tzx->calls = 0;
}

// =============================================================================
// Funciones helper de configuración de bloques (estilo CPCEC)
// =============================================================================

// tzx_tzx20: configura la cola/pausa (equiv. tape_tzx20)
// Convierte tzxhold (ms) en T-states y establece la fase tails.
static void tzx_tzx20(TZXPlayer* t) {
    t->tail  = 0;
    t->tails = 3500 * t->tzxhold; // 1 ms = 3500 T-states
}

// tzx_tzx14: configura la fase de datos (equiv. tape_tzx14)
// Establece codeitem para datos estándar (2 símbolos: bit0 y bit1,
// cada uno con edge type 0 y dos pulsos iguales) y la cola.
static void tzx_tzx14(TZXPlayer* t, int l) {
    t->datacodes = t->time_count = t->item = 0;
    t->mask = t->bits = t->feedable = 1;
    if (t->datas) t->datas -= 8;
    t->datas += l * 8;
    t->codeitem[0][0] = t->codeitem[1][0] = 0; // edge: toggle
    t->codeitem[0][1] = t->codeitem[0][2] = (int16_t)t->tzxbit0;
    t->codeitem[1][1] = t->codeitem[1][2] = (int16_t)t->tzxbit1;
    t->codeitem[0][3] = t->codeitem[1][3] = -1; // end marker
    tzx_tzx20(t);
}

// tzx_tzx12: configura la fase de heads (pilot/sync) (equiv. tape_tzx12)
// Construye headcode[] con entradas de (count, pulse) para pilot y sync.
static void tzx_tzx12(TZXPlayer* t) {
    t->time_count = t->head = t->heads = 0;
    if (t->tzxpilot && t->tzxpilots) {
        t->headcode[0] = t->tzxpilots;
        t->headcode[1] = t->tzxpilot;
        t->heads = 1;
    }
    if (t->tzxsync1 && t->tzxsync2) {
        t->headcode[t->heads * 2]     = 1;
        t->headcode[t->heads * 2 + 1] = t->tzxsync1;
        ++t->heads;
        t->headcode[t->heads * 2]     = 1;
        t->headcode[t->heads * 2 + 1] = t->tzxsync2;
        ++t->heads;
    }
}

// tzx_tzx11: configura heads + datas + tails (equiv. tape_tzx11)
static void tzx_tzx11(TZXPlayer* t, int l) {
    tzx_tzx12(t);
    tzx_tzx14(t, l);
}

// tzx_tzx10: configura bloque estándar (equiv. tape_tzx10)
// q = flag byte & 128 (0 = header → 8062 pilots, 128 = data → 3222 pilots)
static void tzx_tzx10(TZXPlayer* t, int q, int l) {
    t->tzxpilot  = 2168;
    t->tzxpilots = q ? 3222 : 8062;
    t->tzxsync1  = 667;
    t->tzxsync2  = 735;
    t->tzxbit0   = 855;
    t->tzxbit1   = 1710;
    t->datas     = 8; // last_byte_bits para bloque estándar
    tzx_tzx11(t, l);
}

// tzx_tzx19: construye la tabla de codeitem para bloques generalizados
// (equiv. tape_tzx19). Lee n símbolos con m pulsos máximos cada uno.
static void tzx_tzx19(TZXPlayer* t, int n, int m) {
    for (int i = 0, j; i < n; ++i) {
        t->codeitem[i][0] = (int16_t)tzx_getc(t); // edge type
        for (j = 1; j <= m && j < 31; ++j)
            if ((t->codeitem[i][j] = (int16_t)tzx_getcc(t)) < 1)
                t->codeitem[i][j] = -1; // ZERO es marcador de fin anticipado
        for (; j <= m; ++j) tzx_getcc(t); // consume remaining pulses that don't fit
        t->codeitem[i][j < 32 ? j : 31] = -1; // marcador de fin normal
    }
}

// tzx_firstbit: aplica la polaridad del edge (equiv. tape_firstbit)
// m & 0x03: 0=toggle, 1=same, 2=force low, 3=force high
static void tzx_firstbit(TZXPlayer* t, int m) {
    if (m & 2)
        t->ear = (m & 1);
    else if (!(m & 1))
        t->ear ^= 1;
}

// =============================================================================
// Procesamiento de un bloque TZX (equiv. al switch dentro de tape_main)
//
// Lee el bloque actual y configura las fases de reproducción.
// Los bloques de metadatos no configuran fases; el bucle principal
// los procesará consecutivamente hasta encontrar uno que genere señal.
// =============================================================================
static void tzx_process_block(TZXPlayer* t) {
    t->block_idx++;
    if (t->block_idx >= t->block_count) {
        t->active = false;
        printf("[TZX] Fin de cinta.\n");
        return;
    }

    t->read_pos = t->block_offsets[t->block_idx] + 1; // tras el byte de ID
    uint8_t id  = t->block_ids[t->block_idx];
    t->feedable = 0;
    t->blocks_played++;

    printf("[TZX] Bloque %d: 0x%02X %s\n", t->block_idx, id, tzx_block_name(id));

    int p;
    switch (id) {

    // ── 0x10  NORMAL DATA ──────────────────────────────────────────────────
    case 0x10:
        t->tzxhold = tzx_getcc(t);            // pause (ms)
        p = tzx_getcc(t);                     // data length
        tzx_tzx10(t, tzx_getc(t) & 128, p);  // primer byte → flag
        tzx_undo(t);                          // devolver el byte al stream
        break;

    // ── 0x11  CUSTOM DATA ──────────────────────────────────────────────────
    case 0x11:
        t->tzxpilot  = tzx_getcc(t);
        t->tzxsync1  = tzx_getcc(t);
        t->tzxsync2  = tzx_getcc(t);
        t->tzxbit0   = tzx_getcc(t);
        t->tzxbit1   = tzx_getcc(t);
        t->tzxpilots = tzx_getcc(t);
        t->datas     = tzx_getc(t);           // last_byte_bits
        t->tzxhold   = tzx_getcc(t);          // pause (ms)
        tzx_tzx11(t, tzx_getccc(t));          // data length
        break;

    // ── 0x12  PURE TONE ────────────────────────────────────────────────────
    case 0x12:
        t->tzxpilot  = tzx_getcc(t);         // pulse length
        t->tzxpilots = tzx_getcc(t);         // number of pulses
        t->tzxsync1 = t->tzxsync2 = 0;
        tzx_tzx12(t);
        break;

    // ── 0x13  PURE SYNC ────────────────────────────────────────────────────
    case 0x13:
        t->heads = tzx_getc(t);
        for (p = 0; p < t->heads; ++p) {
            t->headcode[p * 2]     = 1;
            t->headcode[p * 2 + 1] = tzx_getcc(t);
        }
        t->time_count = t->head = 0;
        break;

    // ── 0x14  PURE DATA ────────────────────────────────────────────────────
    case 0x14:
        t->tzxbit0 = tzx_getcc(t);
        t->tzxbit1 = tzx_getcc(t);
        t->datas   = tzx_getc(t);            // last_byte_bits
        t->tzxhold = tzx_getcc(t);           // pause (ms)
        tzx_tzx14(t, tzx_getccc(t));         // data length
        break;

    // ── 0x15  SAMPLES ──────────────────────────────────────────────────────
    case 0x15:
        t->time_count = 0;
        t->wave_mask  = tzx_getcc(t);        // T-states per sample
        t->tzxhold    = tzx_getcc(t);        // pause (ms)
        tzx_tzx20(t);
        if ((t->waves = tzx_getc(t)))        // last_byte_bits
            t->waves -= 8;
        t->waves += tzx_getccc(t) << 3;     // + data_len * 8
        break;

    // ── 0x19  GENERAL DATA ─────────────────────────────────────────────────
    case 0x19:
        tzx_getcccc(t);                       // block length (skip)
        t->tzxhold = tzx_getcc(t);           // pause (ms)
        tzx_tzx20(t);
        t->tones     = tzx_getcccc(t);       // TOTP
        t->toneitems = tzx_getc(t);          // NPP
        if (!(t->tonecodes = tzx_getc(t)))   // ASP (0 → 256)
            t->tonecodes = 256;
        t->datas     = tzx_getcccc(t);       // TOTD
        t->dataitems = tzx_getc(t);          // NPD
        if (!(t->datacodes = tzx_getc(t)))   // ASD (0 → 256)
            t->datacodes = 256;
        break;

    // ── 0x20  HOLD (o STOP) ────────────────────────────────────────────────
    case 0x20:
        if ((t->tzxhold = tzx_getcc(t)))
            tzx_tzx20(t);
        else {
            printf("[TZX] Stop the tape.\n");
            t->active = false;
        }
        break;

    // ── 0x2A  STOP ON 48K ──────────────────────────────────────────────────
    case 0x2A:
        tzx_getcccc(t);
        printf("[TZX] Stop if 48K → deteniendo.\n");
        t->active = false;
        break;

    // ── 0x2B  SET SIGNAL LEVEL ─────────────────────────────────────────────
    case 0x2B:
        p = tzx_getcccc(t) - 1;
        t->ear = tzx_getc(t) & 1;
        tzx_skip(t, p);
        break;

    // ── 0x21  GROUP START ──────────────────────────────────────────────────
    case 0x21:
        tzx_skip(t, tzx_getc(t));
        break; // fall-through innecesario; el bucle procesa el siguiente

    // ── 0x22  GROUP END ────────────────────────────────────────────────────
    case 0x22:
        break;

    // ── 0x24  LOOP START ───────────────────────────────────────────────────
    case 0x24:
        t->loops = tzx_getcc(t);
        t->loop_block = t->block_idx;
        break;

    // ── 0x25  LOOP END ─────────────────────────────────────────────────────
    case 0x25:
        if (t->loops && --t->loops)
            t->block_idx = t->loop_block; // process_block incrementará a loop_block+1
        break;

    // ── 0x23  JUMP TO BLOCK ────────────────────────────────────────────────
    case 0x23: {
        int16_t rel = (int16_t)tzx_getcc(t);
        t->block_idx = t->block_idx + (int)rel - 1;
        if (t->block_idx < -1) t->block_idx = -1;
        break;
    }

    // ── 0x26  CALL SEQUENCE ────────────────────────────────────────────────
    case 0x26:
        t->calls = tzx_getcc(t);
        if (t->calls) {
            t->call_block     = t->block_idx;
            t->call_table_pos = t->read_pos + 2; // posición del 2º offset
            int16_t rel = (int16_t)tzx_getcc(t);
            t->block_idx = t->call_block + (int)rel - 1;
        }
        break;

    // ── 0x27  RETURN FROM CALL ─────────────────────────────────────────────
    case 0x27:
        if (t->calls) {
            --t->calls;
            if (t->calls) {
                t->read_pos = t->call_table_pos;
                t->call_table_pos += 2;
                int16_t rel = (int16_t)tzx_getcc(t);
                t->block_idx = t->call_block + (int)rel - 1;
            } else {
                t->block_idx = t->call_block; // volver al bloque tras Call Sequence
            }
        }
        break;

    // ── 0x28  SELECT BLOCK ─────────────────────────────────────────────────
    case 0x28:
        tzx_skip(t, tzx_getcc(t));
        break;

    // ── Bloques informativos (sin señal) ───────────────────────────────────
    case 0x31: // MESSAGE BLOCK
        tzx_getc(t); // caer al siguiente
        // fall through
    case 0x30: // TEXT
        tzx_skip(t, tzx_getc(t));
        break;
    case 0x32: // ARCHIVE INFO
        tzx_skip(t, tzx_getcc(t));
        break;
    case 0x33: // HARDWARE TYPE
        tzx_skip(t, tzx_getc(t) * 3);
        break;
    case 0x34: // EMULATION INFO
        tzx_skip(t, 8);
        break;
    case 0x35: // CUSTOM INFO
        tzx_skip(t, 16);
        tzx_skip(t, tzx_getcccc(t));
        break;
    case 0x40: // SNAPSHOT INFO
        tzx_skip(t, tzx_getcccc(t) >> 8);
        break;
    case 0x5A: // GLUE
        tzx_skip(t, 9);
        break;

    default: // UNKNOWN
        fprintf(stderr, "[TZX] Bloque 0x%02X desconocido, saltando.\n", id);
        tzx_skip(t, tzx_getcccc(t));
        break;
    }
}

// =============================================================================
// API pública: tzx_start y tzx_update
// =============================================================================

void tzx_start(TZXPlayer* t) {
    if (!t->data || t->block_count == 0) {
        fprintf(stderr, "[TZX] No hay datos cargados.\n");
        return;
    }
    t->block_idx  = -1; // process_block incrementará a 0
    t->active     = true;
    t->ear        = 0;
    t->n_acc      = 0;
    t->heads = t->tones = t->datas = t->waves = t->tails = 0;
    t->loops = t->calls = 0;
    t->feedable       = 0;
    t->blocks_played  = 0;
    t->tonecodes = t->datacodes = 0;
    t->time_count = t->item = 0;
    t->head = 0;
    t->tail = 0;
    printf("[TZX] Reproducción iniciada.\n");
}

// =============================================================================
// Bucle principal de reproducción (equiv. tape_main para TZX de CPCEC)
//
// Procesa las fases en orden:
//   heads → tones → datas → waves → tails → (siguiente bloque)
//
// Cada fase añade T-states a n_acc cuando genera un pulso.
// El bucle continúa mientras n_acc <= 0 (= aún hay tiempo que llenar).
// =============================================================================
void tzx_update(TZXPlayer* t, int cycles) {
    if (!t->active) return;

    t->n_acc -= cycles;

    while (t->n_acc <= 0 && t->active) {

        // ── Fase HEADS: pulsos piloto/sync ─────────────────────────────────
        if (t->heads) {
            if (!t->time_count)
                t->time_count = t->headcode[t->head++]; // cargar count
            t->ear ^= 1; // toggle
            t->n_acc += t->headcode[t->head]; // añadir duración del pulso
            if (!--t->time_count)
                ++t->head, --t->heads;
        }

        // ── Fase TONES: símbolos codificados (bloque 0x19 pilot) ───────────
        else if (t->tones) {
            if (t->tonecodes) {
                tzx_tzx19(t, t->tonecodes, t->toneitems);
                t->tonecodes = t->time_count = 0;
            }
            if (!t->time_count) { // nuevo símbolo PRLE
                t->byte_val   = tzx_getc(t);   // symbol index
                t->time_count = tzx_getcc(t);   // repeat count
                t->item       = 0;
            }
            if (!t->item) { // inicio del símbolo
                t->code = t->codeitem[t->byte_val];
                tzx_firstbit(t, *t->code);
            }
            {
                int pulse = *++t->code;
                if (pulse >= 0) {
                    t->n_acc += pulse;
                    if (t->item++) t->ear ^= 1;
                } else {
                    t->item = 0;
                    if (t->time_count > 0 && --t->time_count)
                        t->code = t->codeitem[t->byte_val]; // reiniciar símbolo
                    else
                        --t->tones; // siguiente entrada PRLE
                }
            }
        }

        // ── Fase DATAS: bits/símbolos de datos ─────────────────────────────
        else if (t->datas) {
            if (t->datacodes) {
                int nc = t->datacodes;
                tzx_tzx19(t, nc, t->dataitems);
                // ceil(log2(nc)): bits exactos por símbolo
                { int b = 0, v = nc - 1; while (v > 0) { b++; v >>= 1; } t->bits = b ? b : 1; }
                t->mask = (1 << t->bits) - 1;
                t->feedable = t->bits == 1
                    && !t->codeitem[0][0] && !t->codeitem[1][0]
                    && t->codeitem[0][2] > 0 && t->codeitem[1][2] > 0
                    && t->codeitem[0][3] < 0 && t->codeitem[1][3] < 0;
                t->datacodes = t->time_count = t->item = 0;
            }
            if (!t->time_count) { // nuevo byte
                t->byte_val   = tzx_getc(t);
                t->time_count = 8;
            }
            if (!t->item) { // inicio del símbolo
                int sym;
                if (t->time_count >= t->bits) {
                    sym = (t->byte_val >> (t->time_count - t->bits)) & t->mask;
                } else {
                    // Símbolo a caballo entre dos bytes
                    int have = t->time_count;
                    int need = t->bits - have;
                    sym = (t->byte_val & ((1 << have) - 1)) << need;
                    t->byte_val   = tzx_getc(t);
                    t->time_count = 8 + have; // tras -= bits quedará 8 - need
                    sym |= (t->byte_val >> (8 - need)) & ((1 << need) - 1);
                }
                t->code = t->codeitem[sym];
                tzx_firstbit(t, *t->code);
            }
            {
                int pulse = *++t->code;
                if (pulse >= 0) {
                    t->n_acc += pulse;
                    if (t->item++) t->ear ^= 1;
                } else {
                    t->item = 0;
                    t->time_count -= t->bits;
                    --t->datas;
                }
            }
        }

        // ── Fase WAVES: muestras directas de 1 bit (bloque 0x15) ──────────
        else if (t->waves) {
            if (!t->time_count) {
                t->time_count = 8;
                t->byte_val   = tzx_getc(t);
            }
            t->ear = (t->byte_val >> --t->time_count) & 1;
            t->n_acc += t->wave_mask;
            --t->waves;
        }

        // ── Fase TAILS: cola/pausa al final del bloque ────────────────────
        else if (t->tails) {
            if (t->tails > 3500) {
                t->n_acc += 3500;
                t->tails -= 3500;
            } else {
                t->n_acc += t->tails;
                t->tails = 0;
            }
            if (t->tail) {
                t->ear = 0;
            } else {
                t->tail = 1;
                t->ear ^= 1;
            }
        }

        // ── Siguiente bloque ──────────────────────────────────────────────
        else {
            int watchdog = 99;
            do {
                if (!--watchdog) {
                    t->active = false;
                    fprintf(stderr, "[TZX] Watchdog: cinta corrupta!\n");
                    return;
                }
                tzx_process_block(t);
                if (!t->active) return;
            } while (!(t->heads || t->tones || t->datas || t->waves || t->tails));
        }
    }
}