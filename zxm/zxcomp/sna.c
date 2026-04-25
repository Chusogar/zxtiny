/*
 * sna.c - CPC SNA snapshot loader / saver
 */

#include "sna.h"
#include "cpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNA_MAGIC      "MV - SNA"
#define SNA_HEADER_V1  0x100   /* 256 bytes header */
#define SNA_64K        65536
#define SNA_128K       131072

/* Read little-endian 16-bit */
static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
/* Write little-endian 16-bit */
static inline void w16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = v >> 8;
}

int sna_load(cpc_t *cpc, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "SNA: cannot open %s\n", path); return -1; }

    /* Read header */
    uint8_t hdr[0x100];
    if (fread(hdr, 1, sizeof(hdr), f) < sizeof(hdr)) {
        fprintf(stderr, "SNA: short header\n");
        fclose(f); return -1;
    }

    /* Validate magic */
    if (memcmp(hdr, SNA_MAGIC, 8) != 0) {
        fprintf(stderr, "SNA: invalid magic\n");
        fclose(f); return -1;
    }

    uint8_t version = hdr[0x10];
    printf("SNA: version %d\n", version);

    /* ── Z80 registers ────────────────────────────────────────────── */
    z80_t *cpu = &cpc->cpu;

    cpu->AF.w  = r16(hdr + 0x11);
    cpu->BC.w  = r16(hdr + 0x13);
    cpu->DE.w  = r16(hdr + 0x15);
    cpu->HL.w  = r16(hdr + 0x17);
    cpu->R     = hdr[0x20] & 0x7F;
    cpu->I     = hdr[0x19];        /* I register at 0x19 */
    cpu->IY.w  = r16(hdr + 0x1B);
    cpu->IX.w  = r16(hdr + 0x1D);
    cpu->IFF1  = (hdr[0x1F] >> 0) & 1;
    cpu->IFF2  = (hdr[0x1F] >> 1) & 1;
    cpu->AF_.w = r16(hdr + 0x21);
    cpu->BC_.w = r16(hdr + 0x23);
    cpu->DE_.w = r16(hdr + 0x25);
    cpu->HL_.w = r16(hdr + 0x27);
    cpu->SP    = r16(hdr + 0x29);
    cpu->PC    = r16(hdr + 0x2B);
    cpu->IM    = hdr[0x2D];

    /* ── Gate Array ──────────────────────────────────────────────── */
    gate_array_t *ga = &cpc->gate_array;
    ga->pen_sel = hdr[0x2E] & 0x1F;
    for (int i = 0; i < 17; i++)
        ga->pen_colour[i] = hdr[0x2F + i] & 0x1F;
    uint8_t ga_multi = hdr[0x40];
    ga->mode = ga_multi & 3;
    cpc->mem.lower_rom_en = !(ga_multi & 0x04);
    cpc->mem.upper_rom_en = !(ga_multi & 0x08);

    /* ── RAM config ───────────────────────────────────────────────── */
    cpc->mem.ram_config = hdr[0x41] & 7;

    /* ── CRTC ─────────────────────────────────────────────────────── */
    crtc_t *crtc = &cpc->crtc;
    crtc->addr = hdr[0x42];
    for (int i = 0; i < 18; i++)
        crtc->reg[i] = hdr[0x43 + i];

    /* ── ROM select ───────────────────────────────────────────────── */
    mem_select_rom(&cpc->mem, hdr[0x55]);

    /* ── PPI ──────────────────────────────────────────────────────── */
    cpc->ppi.port_a  = hdr[0x56];
    cpc->ppi.port_b  = hdr[0x57];
    cpc->ppi.port_c  = hdr[0x58];
    cpc->ppi.control = hdr[0x59];

    /* ── PSG ──────────────────────────────────────────────────────── */
    cpc->psg.addr = hdr[0x5A];
    for (int i = 0; i < 16; i++)
        cpc->psg.reg[i] = hdr[0x5B + i];

    /* Re-apply PSG register values */
    for (int i = 0; i < 16; i++) {
        uint8_t saved_addr = cpc->psg.addr;
        cpc->psg.addr = i;
        /* Manually trigger register side-effects */
        switch (i) {
        case 0: case 1: cpc->psg.tone_period[0] = (cpc->psg.reg[1]&0xF)<<8|cpc->psg.reg[0]; break;
        case 2: case 3: cpc->psg.tone_period[1] = (cpc->psg.reg[3]&0xF)<<8|cpc->psg.reg[2]; break;
        case 4: case 5: cpc->psg.tone_period[2] = (cpc->psg.reg[5]&0xF)<<8|cpc->psg.reg[4]; break;
        case 6: cpc->psg.noise_period = cpc->psg.reg[6] & 0x1F; break;
        case 11: case 12: cpc->psg.env_period = (cpc->psg.reg[12]<<8)|cpc->psg.reg[11]; break;
        }
        cpc->psg.addr = saved_addr;
    }

    /* ── Memory dump ─────────────────────────────────────────────── */
    uint16_t mem_kb = r16(hdr + 0x6C);
    if (mem_kb == 0) mem_kb = 64;  /* v1 default */
    size_t mem_size = (size_t)mem_kb * 1024;

    printf("SNA: loading %u KB RAM dump\n", mem_kb);

    if (mem_size > sizeof(cpc->mem.ram)) {
        mem_size = sizeof(cpc->mem.ram);
    }
    if (fread(cpc->mem.ram, 1, mem_size, f) < mem_size) {
        fprintf(stderr, "SNA: warning: short RAM dump\n");
    }

    fclose(f);

    /* Update memory map */
    mem_update_map(&cpc->mem);

    printf("SNA: loaded OK (PC=%04X SP=%04X)\n", cpu->PC, cpu->SP);
    return 0;
}

int sna_save(cpc_t *cpc, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "SNA: cannot write %s\n", path); return -1; }

    uint8_t hdr[0x100];
    memset(hdr, 0, sizeof(hdr));

    /* Magic + version */
    memcpy(hdr, SNA_MAGIC, 8);
    memcpy(hdr + 0x08, "\r\n", 2);
    hdr[0x10] = 3; /* version 3 */

    z80_t *cpu = &cpc->cpu;
    w16(hdr + 0x11, cpu->AF.w);
    w16(hdr + 0x13, cpu->BC.w);
    w16(hdr + 0x15, cpu->DE.w);
    w16(hdr + 0x17, cpu->HL.w);
    hdr[0x19] = cpu->I;
    w16(hdr + 0x1B, cpu->IY.w);
    w16(hdr + 0x1D, cpu->IX.w);
    hdr[0x1F] = (cpu->IFF1 & 1) | ((cpu->IFF2 & 1) << 1);
    hdr[0x20] = cpu->R & 0x7F;
    w16(hdr + 0x21, cpu->AF_.w);
    w16(hdr + 0x23, cpu->BC_.w);
    w16(hdr + 0x25, cpu->DE_.w);
    w16(hdr + 0x27, cpu->HL_.w);
    w16(hdr + 0x29, cpu->SP);
    w16(hdr + 0x2B, cpu->PC);
    hdr[0x2D] = cpu->IM;

    gate_array_t *ga = &cpc->gate_array;
    hdr[0x2E] = ga->pen_sel;
    for (int i = 0; i < 17; i++)
        hdr[0x2F + i] = ga->pen_colour[i];
    hdr[0x40] = ga->mode |
                (!cpc->mem.lower_rom_en ? 0x04 : 0) |
                (!cpc->mem.upper_rom_en ? 0x08 : 0);
    hdr[0x41] = cpc->mem.ram_config;

    crtc_t *crtc = &cpc->crtc;
    hdr[0x42] = crtc->addr;
    for (int i = 0; i < 18; i++)
        hdr[0x43 + i] = crtc->reg[i];

    hdr[0x55] = (uint8_t)cpc->mem.upper_rom_sel;
    hdr[0x56] = cpc->ppi.port_a;
    hdr[0x57] = cpc->ppi.port_b;
    hdr[0x58] = cpc->ppi.port_c;
    hdr[0x59] = cpc->ppi.control;
    hdr[0x5A] = cpc->psg.addr;
    for (int i = 0; i < 16; i++)
        hdr[0x5B + i] = cpc->psg.reg[i];

    /* 128KB dump */
    w16(hdr + 0x6C, 128);

    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(cpc->mem.ram, 1, sizeof(cpc->mem.ram), f);

    fclose(f);
    printf("SNA: saved to %s\n", path);
    return 0;
}
