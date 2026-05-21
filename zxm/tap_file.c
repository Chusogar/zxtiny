#include "tap_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool tap_next_block(TAPPlayer* t) {
    if (!t->data || t->pos + 2 > t->size) {
        t->state = TAP_STATE_IDLE;
        t->active = false;
        printf("[TAP] Fin de cinta.\n");
        return false;
    }

    t->block_len = (uint32_t)t->data[t->pos] | ((uint32_t)t->data[t->pos + 1] << 8);
    t->pos += 2;

    if (t->pos + t->block_len > t->size) {
        t->state = TAP_STATE_IDLE;
        t->active = false;
        printf("[TAP] Bloque truncado.\n");
        return false;
    }

    t->byte_pos = 0;
    t->bit_mask = 0x80;

    uint8_t flag = t->data[t->pos];
    t->pilot_count = (flag == 0x00) ? TAP_PILOT_HEADER : TAP_PILOT_DATA;
    t->state = TAP_STATE_PILOT;
    t->pulse_cycles = TAP_PILOT_PULSE;
    t->ear = 1;

    printf("[TAP] Bloque %u bytes, tipo %s\n",
           (unsigned)t->block_len,
           (flag == 0x00) ? "cabecera" : "datos");

    return true;
}

int tap_file_load(TAPPlayer* t, const char* filename) {
    if (!t || !filename) return -1;

    FILE* f = fopen(filename, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        return -1;
    }

    tap_file_free(t);

    t->data = (uint8_t*)malloc((size_t)sz);
    if (!t->data) {
        fclose(f);
        return -1;
    }

    size_t rd = fread(t->data, 1, (size_t)sz, f);
    fclose(f);

    if (rd != (size_t)sz) {
        tap_file_free(t);
        return -1;
    }

    t->size = (uint32_t)sz;
    t->pos = 0;
    t->active = false;
    t->state = TAP_STATE_IDLE;
    t->ear = 0;

    printf("[TAP] Fichero cargado: %s (%ld bytes)\n", filename, sz);
    return 0;
}

void tap_file_free(TAPPlayer* t) {
    if (!t) return;
    free(t->data);
    memset(t, 0, sizeof(*t));
    t->state = TAP_STATE_IDLE;
}

void tap_file_start(TAPPlayer* t) {
    if (!t || !t->data || t->size == 0) return;

    t->pos = 0;
    t->active = true;
    printf("[TAP] Reproducción iniciada.\n");
    tap_next_block(t);
}

void tap_file_update(TAPPlayer* t, int cycles) {
    if (!t || !t->active || t->state == TAP_STATE_IDLE) return;

    t->pulse_cycles -= cycles;

    while (t->pulse_cycles <= 0) {
        t->ear ^= 1;

        switch (t->state) {
            case TAP_STATE_PILOT:
                t->pilot_count--;
                if (t->pilot_count <= 0) {
                    t->state = TAP_STATE_SYNC1;
                    t->pulse_cycles += TAP_SYNC1_PULSE;
                } else {
                    t->pulse_cycles += TAP_PILOT_PULSE;
                }
                break;

            case TAP_STATE_SYNC1:
                t->state = TAP_STATE_SYNC2;
                t->pulse_cycles += TAP_SYNC2_PULSE;
                break;

            case TAP_STATE_SYNC2:
                t->state = TAP_STATE_DATA;
                t->byte_pos = 0;
                t->bit_mask = 0x80;
                t->pulse_cycles += (t->data[t->pos + t->byte_pos] & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
                break;

            case TAP_STATE_DATA: {
                uint8_t cur_byte = t->data[t->pos + t->byte_pos];
                int pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;

                // Cada bit son dos semi-pulsos: solo avanzamos el bit cuando la señal cae
                if (t->ear == 0) {
                    t->bit_mask >>= 1;
                    if (t->bit_mask == 0) {
                        t->bit_mask = 0x80;
                        t->byte_pos++;

                        if (t->byte_pos >= t->block_len) {
                            t->pos += t->block_len;
                            t->state = TAP_STATE_PAUSE;
                            t->pulse_cycles += TAP_PAUSE_CYCLES;
                            break;
                        }

                        cur_byte = t->data[t->pos + t->byte_pos];
                    }

                    pw = (cur_byte & t->bit_mask) ? TAP_BIT1_PULSE : TAP_BIT0_PULSE;
                }

                t->pulse_cycles += pw;
                break;
            }

            case TAP_STATE_PAUSE:
                t->ear = 0;
                if (!tap_next_block(t)) return;
                break;

            default:
                return;
        }
    }
}
