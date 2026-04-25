/*
 * cpc.h - Amstrad CPC6128 main system header
 */

#ifndef CPC_H
#define CPC_H

#include <stdint.h>
#include "z80.h"
#include "memory.h"
#include "gate_array.h"
#include "psg.h"
#include "fdc.h"
#include "keyboard.h"
#include "crtc.h"
#include "ppi.h"

/* CPC clock: 4 MHz -> 4,000,000 T-states/sec
 * 50 Hz frame: 80,000 T-states per frame
 * PAL: 312 scanlines * ~64us = 312 * 256 T-states = 79,872 */
#define CPC_CLOCK_HZ       4000000
#define CPC_FRAMES_HZ      50
#define CPC_TICKS_PER_FRAME (CPC_CLOCK_HZ / CPC_FRAMES_HZ)

typedef struct cpc_s {
    z80_t          cpu;
    mem_t          mem;
    gate_array_t   gate_array;
    crtc_t         crtc;
    ppi_t          ppi;
    psg_t          psg;
    fdc_t          fdc;
    keyboard_t     keyboard;

    int            warp;           /* warp speed flag */
    uint32_t       tick;           /* total T-states */
    uint8_t        int_counter;    /* gate array interrupt counter */
} cpc_t;

int  cpc_init   (cpc_t *cpc, const char *os_rom, const char *basic_rom, const char *amsdos_rom);
void cpc_reset  (cpc_t *cpc);
void cpc_destroy(cpc_t *cpc);
void cpc_run_frame(cpc_t *cpc);

/* I/O bus */
uint8_t cpc_io_read (z80_t *cpu, uint16_t port);
void    cpc_io_write(z80_t *cpu, uint16_t port, uint8_t val);

/* Memory bus */
uint8_t cpc_mem_read (z80_t *cpu, uint16_t addr);
void    cpc_mem_write(z80_t *cpu, uint16_t addr, uint8_t val);

#endif /* CPC_H */
