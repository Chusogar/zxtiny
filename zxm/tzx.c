// =============================================================================
// tzx.c  –  Reproductor de ficheros TZX para emulador ZX Spectrum 48K
//
// Referencia del formato: https://worldofspectrum.net/TZXformat.html
//   (especificación TZX v1.20)
//
// Arquitectura general
// ────────────────────
// tzx_load()   → lee el fichero, verifica cabecera, construye la tabla de bloques.
// tzx_start()  → rebobina e inicia el primer bloque.
// tzx_update() → llamada cada vez que el Z80 ejecuta instrucciones.
//                Consume 'cycles' T-states y avanza la máquina de pulsos.
//
// La máquina de pulsos genera un nivel EAR (0/1) que cambia en el instante
// exacto (en T-states) que corresponde al estándar ZX Spectrum.  El llamador
// propaga tzx->ear a mic_bit del emulador.
//
// Flujo por bloque
// ────────────────
//  tzx_start_block()  →  decodifica la cabecera del bloque y configura
//                         los parámetros del contexto activo
//  tzx_update()       →  consume ciclos y avanza la máquina de estados;
//                         al llegar a 0 ciclos en el pulso actual llama a
//                         tzx_next_pulse() para obtener el siguiente
//  tzx_next_pulse()   →  avanza dentro del bloque (pilot→sync→data→pause)
//                         o llama a tzx_next_block() al terminar el bloque
//  tzx_next_block()   →  avanza block_idx, gestiona bucles/saltos,
//                         llama a tzx_start_block()
// =============================================================================

#include "tzx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Utilidades de lectura little-endian desde el buffer
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
// Nombres de bloque (para debug/OSD)
// ---------------------------------------------------------------------------
const char* tzx_block_name(uint8_t id) {
    switch (id) {
    case 0x10: return "Standard Speed Data";
    case 0x11: return "Turbo Speed Data";
    case 0x12: return "Pure Tone";
    case 0x13: return "Pulse Sequence";
    case 0x14: return "Pure Data";
    case 0x15: return "Direct Recording";
    case 0x19: return "Generalized Data";
    case 0x20: return "Pause / Stop";
    case 0x21: return "Group Start";
    case 0x22: return "Group End";
    case 0x23: return "Jump To Block";
    case 0x24: return "Loop Start";
    case 0x25: return "Loop End";
    case 0x2A: return "Stop if 48K";
    case 0x30: return "Text Description";
    case 0x31: return "Message Block";
    case 0x32: return "Archive Info";
    case 0x33: return "Hardware Type";
    case 0x35: return "Custom Info";
    case 0x5A: return "Glue Block";
    default:   return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Avance al bloque EAR (toggle del nivel)
// ---------------------------------------------------------------------------
static inline void ear_toggle(TZXPlayer* t) { t->ear ^= 1; }

// ---------------------------------------------------------------------------
// Helpers para Generalized Data Block (0x19)
// ---------------------------------------------------------------------------

// Lee bits_per_sym bits del data stream y devuelve el índice de símbolo
static uint8_t gdb_read_data_symbol(TZXPlayer* t, TZXGDBCtx* g) {
    if (g->bits_per_sym == 0) return 0;
    uint8_t sym = 0;
    for (int i = 0; i < g->bits_per_sym; i++) {
        uint32_t ab  = g->data_bit_pos++;
        uint32_t bi  = ab / 8;
        uint8_t  sh  = 7 - (ab % 8);
        sym = (uint8_t)((sym << 1) | ((t->data[g->data_stream_off + bi] >> sh) & 1));
    }
    return sym;
}

// Offset en data[] de la SYMDEF de un símbolo
static inline uint32_t gdb_symdef_off(TZXGDBCtx* g, uint8_t sym_idx) {
    uint32_t base = (g->phase == 0) ? g->pilot_sym_off  : g->data_sym_off;
    uint8_t  maxp = (g->phase == 0) ? g->npp            : g->npd;
    return base + (uint32_t)sym_idx * (1 + 2 * maxp);
}

// Duración del pulso actual del símbolo actual
static inline uint16_t gdb_cur_pulse_len(TZXPlayer* t, TZXGDBCtx* g) {
    return rd16(t->data + gdb_symdef_off(g, g->cur_sym) + 1 + g->pulse_idx * 2);
}

// Aplica la polaridad de edge para el pulso actual del símbolo.
// Solo el primer pulso (pulse_idx==0) usa el edge type; los demás hacen toggle.
static void gdb_apply_edge(TZXPlayer* t, TZXGDBCtx* g) {
    if (g->pulse_idx == 0) {
        uint8_t edge = t->data[gdb_symdef_off(g, g->cur_sym)] & 0x03;
        switch (edge) {
        case 0:  ear_toggle(t); break;   // opposite (normal edge)
        case 1:  break;                  // same level (no edge)
        case 2:  t->ear = 0; break;      // force low
        case 3:  t->ear = 1; break;      // force high
        }
    } else {
        ear_toggle(t);
    }
}

// =============================================================================
// Construcción de la tabla de bloques
// =============================================================================

// Devuelve el tamaño en bytes del bloque apuntado por 'off' (incluyendo la
// cabecera de ID pero NO el byte de ID en sí, que ya fue consumido).
// Devuelve 0 si el bloque es desconocido o el buffer está incompleto.
static uint32_t tzx_block_body_size(const uint8_t* data, uint32_t size, uint32_t off) {
    if (off >= size) return 0;
    uint8_t id = data[off];
    const uint8_t* p = data + off + 1;  // apunta al primer byte tras el ID
    uint32_t rem = size - off - 1;

    switch (id) {
    case 0x10:  // Standard: 2 pause + 2 len + data
        if (rem < 4) return 0;
        return 4 + rd16(p + 2);
    case 0x11:  // Turbo: 18 bytes cabecera + data
        if (rem < 18) return 0;
        return 18 + rd24(p + 15);
    case 0x12:  // Pure Tone: 2 pulse + 2 count
        return 4;
    case 0x13:  // Pulse Sequence: 1 count + 2*N
        if (rem < 1) return 0;
        return 1 + (uint32_t)p[0] * 2;
    case 0x14:  // Pure Data: 10 cabecera + data
        if (rem < 10) return 0;
        return 10 + rd24(p + 7);
    case 0x15:  // Direct Recording: 8 cabecera + data
        if (rem < 8) return 0;
        return 8 + rd24(p + 5);
    case 0x19:  // Generalized Data: 4 len + body
        if (rem < 4) return 0;
        return 4 + rd32(p);
    case 0x20:  // Pause: 2 bytes
        return 2;
    case 0x21:  // Group Start: 1 len + N chars
        if (rem < 1) return 0;
        return 1 + p[0];
    case 0x22:  // Group End: 0
        return 0;
    case 0x23:  // Jump To Block: 2
        return 2;
    case 0x24:  // Loop Start: 2
        return 2;
    case 0x25:  // Loop End: 0
        return 0;
    case 0x2A:  // Stop if 48K: 4 (reserved)
        return 4;
    case 0x30:  // Text: 1 len + N
        if (rem < 1) return 0;
        return 1 + p[0];
    case 0x31:  // Message: 2 + N
        if (rem < 2) return 0;
        return 2 + p[1];
    case 0x32:  // Archive Info: 2 + N
        if (rem < 2) return 0;
        return 2 + rd16(p);
    case 0x33:  // Hardware Type: 1 + 3*N
        if (rem < 1) return 0;
        return 1 + (uint32_t)p[0] * 3;
    case 0x35:  // Custom Info: 16 + 4 + N
        if (rem < 20) return 0;
        return 20 + rd32(p + 16);
    case 0x5A:  // Glue: 9
        return 9;
    default:
        // Bloques desconocidos con longitud de 4 bytes: intentar saltar
        if (rem < 4) return 0;
        return 4 + rd32(p);
    }
}

// Construye block_offsets[] y block_ids[] recorriendo el buffer lineal.
static int tzx_index_blocks(TZXPlayer* t) {
    // Primera pasada: contar bloques
    int count = 0;
    uint32_t off = 10;  // Cabecera TZX = 10 bytes
    while (off < t->size) {
        uint32_t body = tzx_block_body_size(t->data, t->size, off);
        //if (body == 0 && off < t->size) {
            // Bloque desconocido irrecuperable: parar indexado
        //    break;
        //}
        count++;
        off += 1 + body;
    }

    if (count == 0) return -1;

    t->block_offsets = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    t->block_ids     = (uint8_t*) malloc((size_t)count * sizeof(uint8_t));
    if (!t->block_offsets || !t->block_ids) return -1;

    // Segunda pasada: rellenar tabla
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

    // Verificar cabecera "ZXTape!" + 0x1A
    uint8_t hdr[10];
    fread(hdr, 1, 10, f);
    if (memcmp(hdr, "ZXTape!", 7) != 0 || hdr[7] != 0x1A) {
        fclose(f);
        fprintf(stderr, "[TZX] Cabecera inválida.\n");
        return -1;
    }
    printf("[TZX] Versión %d.%d\n", hdr[8], hdr[9]);

    // Leer fichero completo
    tzx_free(tzx);
    tzx->data = (uint8_t*)malloc((size_t)sz);
    if (!tzx->data) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    fread(tzx->data, 1, (size_t)sz, f);
    fclose(f);
    tzx->size = (uint32_t)sz;

    // Indexar bloques
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

    tzx->active      = false;
    tzx->pulse_state = TZX_PS_IDLE;
    tzx->ear         = 0;
    tzx->block_idx   = 0;
    tzx->loop_depth  = 0;
    return 0;
}

void tzx_free(TZXPlayer* tzx) {
    free(tzx->data);          tzx->data          = NULL;
    free(tzx->block_offsets); tzx->block_offsets = NULL;
    free(tzx->block_ids);     tzx->block_ids     = NULL;
    tzx->size        = 0;
    tzx->block_count = 0;
    tzx->block_idx   = 0;
    tzx->active      = false;
    tzx->pulse_state = TZX_PS_IDLE;
    tzx->loop_depth  = 0;
}

// =============================================================================
// Inicio de bloque: decodifica parámetros y configura la máquina de pulsos
// =============================================================================

// Declaración adelantada
static void tzx_next_block(TZXPlayer* t);

static void tzx_start_block(TZXPlayer* t) {
    if (t->block_idx < 0 || t->block_idx >= t->block_count) {
        // Fin de cinta
        t->pulse_state = TZX_PS_IDLE;
        t->active      = false;
        printf("[TZX] Fin de cinta.\n");
        return;
    }

    uint32_t off = t->block_offsets[t->block_idx];
    uint8_t  id  = t->data[off];
    const uint8_t* p = t->data + off + 1;   // primer byte tras el ID

    printf("[TZX] Bloque %d: 0x%02X %s\n", t->block_idx, id, tzx_block_name(id));
    t->blocks_played++;

    switch (id) {

    // ── 0x10  Standard Speed Data ──────────────────────────────────────────
    case 0x10: {
        uint16_t pause_ms = rd16(p);
        uint16_t data_len = rd16(p + 2);
        uint8_t  flag     = p[4];   // primer byte de datos = flag

        t->dc.pilot_pulse  = TZX_STD_PILOT_PULSE;
        t->dc.sync1_pulse  = TZX_STD_SYNC1;
        t->dc.sync2_pulse  = TZX_STD_SYNC2;
        t->dc.bit0_pulse   = TZX_STD_BIT0;
        t->dc.bit1_pulse   = TZX_STD_BIT1;
        t->dc.pilot_count  = (flag == 0x00) ? TZX_STD_PILOT_HEADER : TZX_STD_PILOT_DATA;
        t->dc.last_byte_bits = 8;
        t->dc.pause_ms     = pause_ms;
        t->dc.data_offset  = off + 5;   // datos empiezan en offset 5 (ID+pause+len = 5)
        t->dc.data_len     = data_len;
        t->dc.byte_pos     = 0;
        t->dc.bit_mask     = 0x80;
        t->dc.bits_left    = 8;
        t->dc.half_pulse   = 0;

        t->pulse_state  = TZX_PS_PILOT;
        t->pulse_cycles = t->dc.pilot_pulse;
        t->ear          = 1;
        break;
    }

    // ── 0x11  Turbo Speed Data ─────────────────────────────────────────────
    case 0x11: {
        t->dc.pilot_pulse    = rd16(p);
        t->dc.sync1_pulse    = rd16(p + 2);
        t->dc.sync2_pulse    = rd16(p + 4);
        t->dc.bit0_pulse     = rd16(p + 6);
        t->dc.bit1_pulse     = rd16(p + 8);
        t->dc.pilot_count    = rd16(p + 10);
        t->dc.last_byte_bits = p[12];
        uint16_t pause_ms    = rd16(p + 13);
        uint32_t data_len    = rd24(p + 15);

        t->dc.pause_ms    = pause_ms;
        t->dc.data_offset = off + 19;  // ID(1) + 18 bytes cabecera
        t->dc.data_len    = data_len;
        t->dc.byte_pos    = 0;
        t->dc.bit_mask    = 0x80;
        t->dc.bits_left   = (data_len <= 1) ? t->dc.last_byte_bits : 8;
        t->dc.half_pulse  = 0;

        t->pulse_state  = TZX_PS_PILOT;
        t->pulse_cycles = t->dc.pilot_pulse;
        t->ear          = 1;
        break;
    }

    // ── 0x12  Pure Tone ────────────────────────────────────────────────────
    case 0x12: {
        uint16_t pulse_len = rd16(p);
        uint16_t num_pulses= rd16(p + 2);
        t->ps.pulses[0] = pulse_len;
        t->ps.count     = num_pulses;
        t->ps.index     = 0;

        t->pulse_state  = TZX_PS_PURE_TONE;
        t->pulse_cycles = pulse_len;
        t->ear          = 1;
        break;
    }

    // ── 0x13  Pulse Sequence ───────────────────────────────────────────────
    case 0x13: {
        int n = p[0];
        if (n > TZX_MAX_PULSES) n = TZX_MAX_PULSES;
        for (int i = 0; i < n; i++)
            t->ps.pulses[i] = rd16(p + 1 + i * 2);
        t->ps.count    = n;
        t->ps.index    = 0;

        if (n == 0) { tzx_next_block(t); return; }

        t->pulse_state  = TZX_PS_PULSE_SEQ;
        t->pulse_cycles = t->ps.pulses[0];
        ear_toggle(t);   // El primer pulso invierte el nivel actual
        break;
    }

    // ── 0x14  Pure Data ────────────────────────────────────────────────────
    case 0x14: {
        t->dc.bit0_pulse     = rd16(p);
        t->dc.bit1_pulse     = rd16(p + 2);
        t->dc.last_byte_bits = p[4];
        uint16_t pause_ms    = rd16(p + 5);
        uint32_t data_len    = rd24(p + 7);

        // Sin pilot ni sync
        t->dc.pilot_pulse  = 0;
        t->dc.sync1_pulse  = 0;
        t->dc.sync2_pulse  = 0;
        t->dc.pilot_count  = 0;
        t->dc.pause_ms     = pause_ms;
        t->dc.data_offset  = off + 11;  // ID(1) + 10 bytes cabecera
        t->dc.data_len     = data_len;
        t->dc.byte_pos     = 0;
        t->dc.bit_mask     = 0x80;
        t->dc.bits_left    = (data_len <= 1) ? t->dc.last_byte_bits : 8;
        t->dc.half_pulse   = 0;

        // Empezar directamente en DATA (sin pilot/sync)
        t->pulse_state = TZX_PS_PURE_DATA;
        // El primer pulso del primer bit
        {
            uint8_t byte0 = t->data[t->dc.data_offset];
            int pw = (byte0 & 0x80) ? t->dc.bit1_pulse : t->dc.bit0_pulse;
            t->pulse_cycles = pw;
        }
        ear_toggle(t);
        break;
    }

    // ── 0x15  Direct Recording ─────────────────────────────────────────────
    case 0x15: {
        t->dr.tstates_per_sample = rd16(p);
        uint16_t pause_ms        = rd16(p + 2);
        t->dr.last_byte_bits     = p[4];
        uint32_t data_len        = rd24(p + 5);

        t->dr.data_offset   = off + 9;  // ID(1) + 8 cabecera
        t->dr.pause_ms      = pause_ms;
        t->dr.sample_idx    = 0;
        t->dr.sample_cycles = t->dr.tstates_per_sample;
        // Total de muestras = (data_len-1)*8 + last_byte_bits
        t->dr.total_samples = (data_len > 0)
            ? ((data_len - 1) * 8 + t->dr.last_byte_bits) : 0;

        t->pulse_state  = TZX_PS_DIRECT;
        t->pulse_cycles = t->dr.tstates_per_sample;
        // Nivel inicial del primer sample
        {
            uint8_t byte0 = (data_len > 0) ? t->data[t->dr.data_offset] : 0;
            t->ear = (byte0 & 0x80) ? 1 : 0;
        }
        break;
    }

    // ── 0x19  Generalized Data ───────────────────────────────────────────────
    case 0x19: {
        // p apunta al primer byte tras el ID (o sea, al DWORD block_length)
        // Cabecera fija: 4 block_len + 2 pause + 4 TOTP + 1 NPP + 1 ASP
        //              + 4 TOTD + 1 NPD + 1 ASD = 18 bytes
        if (rd32(p) < 14) { tzx_next_block(t); return; }

        TZXGDBCtx *g = &t->gdb;
        g->pause_ms = rd16(p + 4);
        g->totp     = rd32(p + 6);
        g->npp      = p[10];
        g->asp      = p[11] ? p[11] : 256;
        g->totd     = rd32(p + 12);
        g->npd      = p[16];
        g->asd      = p[17] ? p[17] : 256;

        // ceil(log2(ASD))
        {
            uint16_t n = g->asd;
            uint8_t b = 0;
            if (n > 1) { n--; while (n) { b++; n >>= 1; } }
            g->bits_per_sym = b;
        }

        // Calcular offsets a las secciones del bloque
        uint32_t cur = off + 1 + 18; // tras ID + 18 bytes de cabecera
        if (g->totp > 0) {
            g->pilot_sym_off  = cur;
            cur += (uint32_t)g->asp * (1 + 2 * g->npp);
            g->pilot_prle_off = cur;
            cur += g->totp * 3;
        } else {
            g->pilot_sym_off  = 0;
            g->pilot_prle_off = 0;
        }
        if (g->totd > 0) {
            g->data_sym_off    = cur;
            cur += (uint32_t)g->asd * (1 + 2 * g->npd);
            g->data_stream_off = cur;
        } else {
            g->data_sym_off    = 0;
            g->data_stream_off = 0;
        }

        // Iniciar la reproducción
        g->pulse_idx     = 0;
        g->prle_idx      = 0;
        g->sym_rep       = 0;
        g->data_sym_done = 0;
        g->data_bit_pos  = 0;

        if (g->totp > 0) {
            g->phase    = 0; // piloto
            g->cur_sym  = t->data[g->pilot_prle_off];
            g->cur_reps = rd16(t->data + g->pilot_prle_off + 1);
        } else if (g->totd > 0) {
            g->phase = 1; // datos
            // Leer primer símbolo del data stream
            g->cur_sym = gdb_read_data_symbol(t, g);
        } else {
            // Ni piloto ni datos → solo pausa
            if (g->pause_ms > 0) {
                t->pulse_state   = TZX_PS_PAUSE;
                t->pulse_cycles  = (int32_t)TZX_MS_TO_TSTATES(g->pause_ms);
                t->ear           = 0;
            } else {
                tzx_next_block(t);
            }
            return;
        }

        // Emitir el primer pulso del primer símbolo
        t->pulse_state = TZX_PS_GDB;
        gdb_apply_edge(t, g);
        t->pulse_cycles = gdb_cur_pulse_len(t, g);
        break;
    }

    // ── 0x20  Pause / Stop ────────────────────────────────────────────────
    case 0x20: {
        uint16_t pause_ms = rd16(p);
        if (pause_ms == 0) {
            // Stop the tape
            printf("[TZX] Stop the tape.\n");
            t->pulse_state = TZX_PS_IDLE;
            t->active      = false;
        } else {
            t->pulse_state  = TZX_PS_PAUSE;
            t->pulse_cycles = (int32_t)TZX_MS_TO_TSTATES(pause_ms);
            t->ear          = 0;
        }
        break;
    }

    // ── 0x21  Group Start / 0x22 Group End ────────────────────────────────
    case 0x21:
    case 0x22:
        tzx_next_block(t);
        return;

    // ── 0x23  Jump To Block ────────────────────────────────────────────────
    case 0x23: {
        // El valor es un offset relativo con signo desde el bloque SIGUIENTE
        int16_t jump = (int16_t)rd16(p);
        // jump=1 → siguiente (normal), jump=-1 → bloque anterior
        t->block_idx += (int)jump - 1;  // -1 porque tzx_next_block hará +1
        if (t->block_idx < -1) t->block_idx = -1;
        tzx_next_block(t);
        return;
    }

    // ── 0x24  Loop Start ──────────────────────────────────────────────────
    case 0x24: {
        uint16_t reps = rd16(p);
        if (t->loop_depth < TZX_MAX_LOOP_DEPTH) {
            t->loop_block[t->loop_depth] = t->block_idx + 1;
            t->loop_count[t->loop_depth] = (int)reps;
            t->loop_depth++;
        } else {
            fprintf(stderr, "[TZX] Desbordamiento de pila de bucles.\n");
        }
        tzx_next_block(t);
        return;
    }

    // ── 0x25  Loop End ────────────────────────────────────────────────────
    case 0x25: {
        if (t->loop_depth > 0) {
            int d = t->loop_depth - 1;
            t->loop_count[d]--;
            if (t->loop_count[d] > 0) {
                // Volver al bloque de inicio del bucle
                t->block_idx = t->loop_block[d] - 1;  // -1 porque next_block hará +1
            } else {
                // Bucle terminado
                t->loop_depth--;
            }
        }
        tzx_next_block(t);
        return;
    }

    // ── 0x2A  Stop if 48K ─────────────────────────────────────────────────
    case 0x2A:
        printf("[TZX] Stop if 48K → deteniendo.\n");
        t->pulse_state = TZX_PS_IDLE;
        t->active      = false;
        return;

    // ── Bloques de metadatos (ignorar y avanzar) ───────────────────────────
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x35: case 0x5A:
        tzx_next_block(t);
        return;

    default:
        fprintf(stderr, "[TZX] Bloque 0x%02X no implementado, saltando.\n", id);
        tzx_next_block(t);
        return;
    }
}

// =============================================================================
// Avance al siguiente bloque
// =============================================================================

static void tzx_next_block(TZXPlayer* t) {
    t->block_idx++;
    if (t->block_idx >= t->block_count) {
        t->pulse_state = TZX_PS_IDLE;
        t->active      = false;
        printf("[TZX] Fin de cinta (bloque %d).\n", t->block_idx - 1);
        return;
    }
    tzx_start_block(t);
}

// =============================================================================
// Máquina de pulsos: obtener el siguiente pulso
//
// Se llama cuando pulse_cycles <= 0, es decir, el pulso actual ha terminado.
// Devuelve la duración del siguiente pulso (en T-states) y actualiza el estado.
// =============================================================================

static void tzx_next_pulse(TZXPlayer* t) {
    // El toggle del EAR ya se hará dentro de cada caso según corresponda

    switch (t->pulse_state) {

    // ── Tono piloto ─────────────────────────────────────────────────────────
    case TZX_PS_PILOT: {
        ear_toggle(t);
        t->dc.pilot_count--;
        if (t->dc.pilot_count > 0) {
            t->pulse_cycles += t->dc.pilot_pulse;
        } else {
            // Fin del pilot → sync1
            t->pulse_state   = TZX_PS_SYNC1;
            t->pulse_cycles += t->dc.sync1_pulse;
        }
        break;
    }

    // ── Sync 1 ──────────────────────────────────────────────────────────────
    case TZX_PS_SYNC1:
        ear_toggle(t);
        t->pulse_state   = TZX_PS_SYNC2;
        t->pulse_cycles += t->dc.sync2_pulse;
        break;

    // ── Sync 2 → Data ───────────────────────────────────────────────────────
    case TZX_PS_SYNC2:
        ear_toggle(t);
        t->pulse_state = TZX_PS_DATA;
        goto emit_data_bit;

    // ── Datos (0x10, 0x11) ──────────────────────────────────────────────────
    case TZX_PS_DATA: {
        ear_toggle(t);
        t->dc.half_pulse ^= 1;
        if (t->dc.half_pulse == 0) {
            t->dc.bit_mask >>= 1;
            t->dc.bits_left--;
            if (t->dc.bits_left == 0) {
                // Todos los bits válidos de este byte procesados
                t->dc.byte_pos++;
                t->dc.bit_mask = 0x80;
                if (t->dc.byte_pos >= t->dc.data_len) {
                    // Fin de bloque → pausa
                    goto end_of_data;
                }
                // Ajustar bits válidos para el siguiente byte
                t->dc.bits_left = (t->dc.byte_pos == t->dc.data_len - 1)
                                   ? t->dc.last_byte_bits : 8;
            }
        }
        // Siguiente semi-pulso del bit actual
        emit_data_bit: {
            uint8_t byte = t->data[t->dc.data_offset + t->dc.byte_pos];
            int pw = (byte & t->dc.bit_mask) ? t->dc.bit1_pulse : t->dc.bit0_pulse;
            t->pulse_cycles += pw;
        }
        break;

        end_of_data:
        // Emitir pausa al final del bloque de datos
        if (t->dc.pause_ms > 0) {
            t->pulse_state   = TZX_PS_PAUSE;
            t->pulse_cycles += (int32_t)TZX_MS_TO_TSTATES(t->dc.pause_ms);
            t->ear           = 0;
        } else {
            tzx_next_block(t);
        }
        break;
    }

    // ── Pure Tone (0x12) ────────────────────────────────────────────────────
    case TZX_PS_PURE_TONE:
        ear_toggle(t);
        t->ps.index++;
        if (t->ps.index >= t->ps.count) {
            tzx_next_block(t);
        } else {
            t->pulse_cycles += t->ps.pulses[0];  // todos los pulsos tienen la misma duración
        }
        break;

    // ── Pulse Sequence (0x13) ───────────────────────────────────────────────
    case TZX_PS_PULSE_SEQ:
        ear_toggle(t);
        t->ps.index++;
        if (t->ps.index >= t->ps.count) {
            tzx_next_block(t);
        } else {
            t->pulse_cycles += t->ps.pulses[t->ps.index];
        }
        break;

    // ── Pure Data (0x14) ────────────────────────────────────────────────────
    case TZX_PS_PURE_DATA: {
        ear_toggle(t);
        t->dc.half_pulse ^= 1;
        if (t->dc.half_pulse == 0) {
            // Segundo semi-pulso terminado → avanzar bit
            t->dc.bit_mask >>= 1;
            t->dc.bits_left--;
            if (t->dc.bits_left == 0) {
                t->dc.byte_pos++;
                t->dc.bit_mask = 0x80;
                if (t->dc.byte_pos >= t->dc.data_len) {
                    if (t->dc.pause_ms > 0) {
                        t->pulse_state   = TZX_PS_PAUSE;
                        t->pulse_cycles += (int32_t)TZX_MS_TO_TSTATES(t->dc.pause_ms);
                        t->ear           = 0;
                    } else {
                        tzx_next_block(t);
                    }
                    break;
                }
                t->dc.bits_left = (t->dc.byte_pos == t->dc.data_len - 1)
                                   ? t->dc.last_byte_bits : 8;
            }
        }
        {
            uint8_t byte = t->data[t->dc.data_offset + t->dc.byte_pos];
            int pw = (byte & t->dc.bit_mask) ? t->dc.bit1_pulse : t->dc.bit0_pulse;
            t->pulse_cycles += pw;
        }
        break;
    }

    // ── Direct Recording (0x15) ─────────────────────────────────────────────
    case TZX_PS_DIRECT: {
        t->dr.sample_idx++;
        if (t->dr.sample_idx >= t->dr.total_samples) {
            if (t->dr.pause_ms > 0) {
                t->pulse_state   = TZX_PS_PAUSE;
                t->pulse_cycles += (int32_t)TZX_MS_TO_TSTATES(t->dr.pause_ms);
                t->ear           = 0;
            } else {
                tzx_next_block(t);
            }
            break;
        }
        // Leer nivel del siguiente sample
        uint32_t byte_idx = t->dr.sample_idx / 8;
        uint8_t  bit_idx  = (uint8_t)(7 - (t->dr.sample_idx % 8));
        uint8_t  byte     = t->data[t->dr.data_offset + byte_idx];
        t->ear            = (byte >> bit_idx) & 1;
        t->pulse_cycles  += t->dr.tstates_per_sample;
        break;
    }

    // ── Generalized Data (0x19) ─────────────────────────────────────────────
    case TZX_PS_GDB: {
        TZXGDBCtx *g = &t->gdb;
        uint8_t maxp = (g->phase == 0) ? g->npp : g->npd;

        // Avanzar al siguiente pulso dentro del símbolo actual
        g->pulse_idx++;

        // ¿Símbolo completado? (max pulsos alcanzado o pulso con duración 0)
        bool sym_done = (g->pulse_idx >= maxp);
        if (!sym_done && gdb_cur_pulse_len(t, g) == 0)
            sym_done = true;

        if (sym_done) {
            // Símbolo completado → avanzar al siguiente
            if (g->phase == 0) {
                // Fase piloto: avanzar repetición / PRLE
                g->sym_rep++;
                if (g->sym_rep >= g->cur_reps) {
                    // Todas las repeticiones de este PRLE hechas
                    g->prle_idx++;
                    if (g->prle_idx >= g->totp) {
                        // Piloto terminado → pasar a datos
                        if (g->totd > 0) {
                            g->phase        = 1;
                            g->data_sym_done = 0;
                            g->data_bit_pos  = 0;
                            g->cur_sym       = gdb_read_data_symbol(t, g);
                            g->pulse_idx     = 0;
                            gdb_apply_edge(t, g);
                            t->pulse_cycles += gdb_cur_pulse_len(t, g);
                            break;
                        }
                        goto gdb_end_block;
                    }
                    // Cargar siguiente PRLE
                    uint32_t po   = g->pilot_prle_off + g->prle_idx * 3;
                    g->cur_sym    = t->data[po];
                    g->cur_reps   = rd16(t->data + po + 1);
                    g->sym_rep    = 0;
                }
                g->pulse_idx = 0;
            } else {
                // Fase datos: siguiente símbolo
                g->data_sym_done++;
                if (g->data_sym_done >= g->totd) {
                    goto gdb_end_block;
                }
                g->cur_sym   = gdb_read_data_symbol(t, g);
                g->pulse_idx = 0;
            }
            // Emitir primer pulso del nuevo símbolo
            gdb_apply_edge(t, g);
            t->pulse_cycles += gdb_cur_pulse_len(t, g);
            break;

            gdb_end_block:
            if (g->pause_ms > 0) {
                t->pulse_state   = TZX_PS_PAUSE;
                t->pulse_cycles += (int32_t)TZX_MS_TO_TSTATES(g->pause_ms);
                t->ear           = 0;
            } else {
                tzx_next_block(t);
            }
            break;
        }

        // Pulso intermedio dentro del símbolo actual (no es el primero)
        gdb_apply_edge(t, g);
        t->pulse_cycles += gdb_cur_pulse_len(t, g);
        break;
    }

    // ── Pausa ────────────────────────────────────────────────────────────────
    case TZX_PS_PAUSE:
        t->ear = 0;
        tzx_next_block(t);
        break;

    default:
        t->pulse_state = TZX_PS_IDLE;
        t->active      = false;
        break;
    }
}

// =============================================================================
// API pública: tzx_start y tzx_update
// =============================================================================

void tzx_start(TZXPlayer* tzx) {
    if (!tzx->data || tzx->block_count == 0) {
        fprintf(stderr, "[TZX] No hay datos cargados.\n");
        return;
    }
    tzx->block_idx   = 0;
    tzx->active      = true;
    tzx->ear         = 0;
    tzx->pulse_state = TZX_PS_IDLE;
    tzx->loop_depth  = 0;
    tzx->blocks_played = 0;
    printf("[TZX] Reproducción iniciada.\n");
    tzx_start_block(tzx);
}

void tzx_update(TZXPlayer* tzx, int cycles) {
    if (!tzx->active || tzx->pulse_state == TZX_PS_IDLE) return;

    tzx->pulse_cycles -= cycles;

    // Mientras haya pulsos que despachar
    while (tzx->pulse_cycles <= 0) {
        if (!tzx->active || tzx->pulse_state == TZX_PS_IDLE) break;
        tzx_next_pulse(tzx);
    }
}