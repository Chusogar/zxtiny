/*
 * cpc.c - Amstrad CPC6128 system implementation
 */

#include "cpc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Memory callbacks ──────────────────────────────────────────────────── */
uint8_t cpc_mem_read(z80_t *cpu, uint16_t addr)
{
    cpc_t *cpc = (cpc_t *)cpu->userdata;
    return mem_read(&cpc->mem, addr);
}

void cpc_mem_write(z80_t *cpu, uint16_t addr, uint8_t val)
{
    cpc_t *cpc = (cpc_t *)cpu->userdata;
    mem_write(&cpc->mem, addr, val);
}

/* ── I/O callbacks ──────────────────────────────────────────────────────── */
uint8_t cpc_io_read(z80_t *cpu, uint16_t port)
{
    cpc_t *cpc = (cpc_t *)cpu->userdata;
    uint8_t val = 0xFF;

    /* CPC I/O decoding: active LOW, upper address bits select device */

    /* CRTC (A14=0, A9=1, A8=0) */
    if (!(port & 0x4000) && (port & 0x0200) && !(port & 0x0100)) {
        val = crtc_read(&cpc->crtc, (port >> 9) & 1);
    }

    /* PPI (A11=0) */
    if (!(port & 0x0800)) {
        val = ppi_read(&cpc->ppi, cpc, (port >> 8) & 3);
    }

    /* FDC (A10=0, A7=0) */
    if (!(port & 0x0400) && !(port & 0x0080)) {
        val = fdc_read(&cpc->fdc, port & 1);
    }

    return val;
}

void cpc_io_write(z80_t *cpu, uint16_t port, uint8_t val)
{
    cpc_t *cpc = (cpc_t *)cpu->userdata;

    /* Gate Array (A15=1, A14=0) */
    if ((port & 0xC000) == 0x4000) {
        gate_array_write(&cpc->gate_array, &cpc->mem, val);
        return;
    }

    /* CRTC (A14=0, A9=1) */
    if (!(port & 0x4000) && (port & 0x0200)) {
        crtc_write(&cpc->crtc, (port >> 8) & 1, val);
        return;
    }

    /* ROM banking (A13=0) */
    if (!(port & 0x2000)) {
        mem_select_rom(&cpc->mem, val);
        return;
    }

    /* PPI (A11=0) */
    if (!(port & 0x0800)) {
        ppi_write(&cpc->ppi, cpc, (port >> 8) & 3, val);
        return;
    }

    /* FDC (A10=0, A7=0) */
    if (!(port & 0x0400) && !(port & 0x0080)) {
        fdc_write(&cpc->fdc, port & 1, val);
        return;
    }
}

/* ── Init ─────────────────────────────────────────────────────────────── */
int cpc_init(cpc_t *cpc, const char *os_rom, const char *basic_rom, const char *amsdos_rom)
{
    memset(cpc, 0, sizeof(*cpc));

    /* Load ROMs */
    if (mem_init(&cpc->mem, os_rom, basic_rom, amsdos_rom) != 0) {
        fprintf(stderr, "ROM load failed\n");
        return -1;
    }

    /* Z80 */
    z80_init(&cpc->cpu);
    cpc->cpu.userdata  = cpc;
    cpc->cpu.mem_read  = cpc_mem_read;
    cpc->cpu.mem_write = cpc_mem_write;
    cpc->cpu.io_read   = cpc_io_read;
    cpc->cpu.io_write  = cpc_io_write;

    /* Chips */
    gate_array_init(&cpc->gate_array);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
    fdc_init(&cpc->fdc);
    keyboard_init(&cpc->keyboard);

    printf("CPC6128 initialised\n");
    return 0;
}

void cpc_reset(cpc_t *cpc)
{
    z80_reset(&cpc->cpu);
    gate_array_init(&cpc->gate_array);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    fdc_init(&cpc->fdc);
    cpc->int_counter = 0;
    printf("CPC reset\n");
}

void cpc_destroy(cpc_t *cpc)
{
    mem_destroy(&cpc->mem);
    fdc_destroy(&cpc->fdc);
    psg_destroy(&cpc->psg);
}

/* ── Run one video frame (50 Hz = 79,872 T-states) ───────────────────── */
void cpc_run_frame(cpc_t *cpc)
{
    /* CPC has 312 scanlines, each 64us = 256 T-states */
    /* HSYNC triggers Gate Array interrupt every 52 lines -> every 13312 T */
    int total = CPC_TICKS_PER_FRAME;
    int done  = 0;

    /* Scanline granularity: 256 T-states per line */
    while (done < total) {
        int line_ticks = 0;

        /* Execute one scanline worth of Z80 */
        while (line_ticks < 256) {
            int t = z80_step(&cpc->cpu);
            line_ticks += t;

            /* Tick FDC at 1 MHz (every 4 T-states) */
            for (int i = 0; i < t; i += 4) {
                fdc_tick(&cpc->fdc);
            }

            /* PSG audio sample generation */
            psg_tick(&cpc->psg, t);
        }

        /* CRTC scanline */
        int vsync = crtc_scanline(&cpc->crtc);

        /* Gate Array: render scanline to framebuffer */
        gate_array_scanline(&cpc->gate_array, &cpc->crtc, &cpc->mem,
                            cpc->crtc.scanline_count);

        /* Gate Array interrupt: every 52 HSYNC */
        cpc->int_counter++;
        if (cpc->int_counter >= 52) {
            cpc->int_counter = 0;
            z80_set_int(&cpc->cpu, 1);
        }

        if (vsync) {
            /* Reset interrupt counter at VSYNC */
            if (cpc->int_counter >= 32) {
                cpc->int_counter = 0;
                z80_set_int(&cpc->cpu, 1);
            }
        }

        /* Clear interrupt after one T-state ack */
        z80_set_int(&cpc->cpu, 0);

        done += line_ticks;
    }

    cpc->tick += done;
}
