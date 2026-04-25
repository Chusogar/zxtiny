/*
 * memory.c - CPC6128 memory subsystem
 */

#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_rom(uint8_t *dst, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open ROM: %s\n", path);
        return -1;
    }
    size_t n = fread(dst, 1, ROM_SIZE, f);
    fclose(f);
    if (n < ROM_SIZE) {
        fprintf(stderr, "Short ROM read (%zu/%d): %s\n", n, ROM_SIZE, path);
        /* pad with 0xFF */
        memset(dst + n, 0xFF, ROM_SIZE - n);
    }
    printf("Loaded ROM %s (%zu bytes)\n", path, n);
    return 0;
}

int mem_init(mem_t *mem, const char *os, const char *basic, const char *amsdos)
{
    memset(mem, 0, sizeof(*mem));
    memset(mem->ram, 0x00, sizeof(mem->ram));

    /* Load mandatory ROMs */
    if (os) {
        if (load_rom(mem->rom[ROM_OS], os) == 0)
            mem->rom_loaded[ROM_OS] = 1;
    }
    if (basic) {
        if (load_rom(mem->rom[ROM_BASIC], basic) == 0)
            mem->rom_loaded[ROM_BASIC] = 1;
    }
    if (amsdos) {
        if (load_rom(mem->rom[ROM_AMSDOS], amsdos) == 0)
            mem->rom_loaded[ROM_AMSDOS] = 1;
    }

    mem->lower_rom_en  = 1;
    mem->upper_rom_en  = 1;
    mem->upper_rom_sel = ROM_BASIC;
    mem->ram_config    = 0;

    mem_update_map(mem);
    return 0;
}

void mem_destroy(mem_t *mem)
{
    (void)mem;
}

/*
 * CPC6128 RAM banking (Gate Array register 0xC0xx):
 *
 * bits 2:0:
 *   000: bank0=0, bank1=1, bank2=2, bank3=3  (standard)
 *   001: bank0=0, bank1=1, bank2=2, bank3=7
 *   010: bank0=4, bank1=5, bank2=6, bank3=7
 *   011: bank0=0, bank1=3, bank2=2, bank3=7
 *   100: bank0=0, bank1=4, bank2=2, bank3=3
 *   101: bank0=0, bank1=5, bank2=2, bank3=3
 *   110: bank0=0, bank1=6, bank2=2, bank3=3
 *   111: bank0=0, bank1=7, bank2=2, bank3=3
 *
 * Each bank = 16KB, 8 banks total in 128KB
 */
static const uint8_t ram_banks[8][4] = {
    {0, 1, 2, 3},
    {0, 1, 2, 7},
    {4, 5, 6, 7},
    {0, 3, 2, 7},
    {0, 4, 2, 3},
    {0, 5, 2, 3},
    {0, 6, 2, 3},
    {0, 7, 2, 3},
};

void mem_update_map(mem_t *mem)
{
    uint8_t cfg = mem->ram_config & 7;
    const uint8_t *banks = ram_banks[cfg];

    for (int page = 0; page < 4; page++) {
        uint32_t bank_offset = banks[page] * PAGE_SIZE;
        /* Write always goes to RAM */
        mem->write_page[page] = mem->ram + bank_offset;
        /* Read: page 0 = OS ROM if enabled, page 3 = upper ROM if enabled */
        mem->read_page[page]  = mem->ram + bank_offset;
    }

    /* Lower ROM overlay (0x0000-0x3FFF) */
    if (mem->lower_rom_en && mem->rom_loaded[ROM_OS]) {
        mem->read_page[0] = mem->rom[ROM_OS];
    }

    /* Upper ROM overlay (0xC000-0xFFFF) */
    if (mem->upper_rom_en) {
        int sel = mem->upper_rom_sel;
        if (sel < ROM_COUNT && mem->rom_loaded[sel]) {
            mem->read_page[3] = mem->rom[sel];
        }
    }
}

void mem_select_rom(mem_t *mem, uint8_t rom_sel)
{
    /* Upper ROM select: 0=BASIC, 7=AMSDOS */
    if (rom_sel == 7 && mem->rom_loaded[ROM_AMSDOS]) {
        mem->upper_rom_sel = ROM_AMSDOS;
    } else if (rom_sel == 0 && mem->rom_loaded[ROM_BASIC]) {
        mem->upper_rom_sel = ROM_BASIC;
    } else {
        /* Try to use BASIC as fallback */
        mem->upper_rom_sel = ROM_BASIC;
    }
    mem_update_map(mem);
}
