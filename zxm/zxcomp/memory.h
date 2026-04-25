/*
 * memory.h - CPC6128 memory map
 *
 * CPC6128: 128KB RAM in 4x32KB banks
 * ROMs: OS (16KB), BASIC (16KB), AMSDOS (16KB)
 * Gate Array controls ROM enable + RAM banking
 */

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

#define RAM_SIZE     (128 * 1024)
#define ROM_SIZE     (16  * 1024)
#define PAGE_SIZE    (16  * 1024)   /* 16KB pages */

/* ROM slots */
#define ROM_OS      0
#define ROM_BASIC   1
#define ROM_AMSDOS  2
#define ROM_COUNT   8               /* up to 8 upper ROMs */

typedef struct mem_s {
    uint8_t  ram[RAM_SIZE];

    uint8_t  rom[ROM_COUNT][ROM_SIZE];
    int      rom_loaded[ROM_COUNT];

    /* Current bank configuration (Gate Array RAM config register) */
    uint8_t  ram_config;     /* bits 2:0 of gate array RAM banking byte */

    /* ROM enables */
    uint8_t  lower_rom_en;   /* 1 = OS ROM at 0x0000-0x3FFF */
    uint8_t  upper_rom_en;   /* 1 = BASIC/AMSDOS at 0xC000-0xFFFF */
    uint8_t  upper_rom_sel;  /* which upper ROM is visible */

    /* Resolved page pointers [4 pages of 16KB] */
    /* read  pages: can point into rom[] or ram[] */
    /* write pages: always into ram[]             */
    uint8_t *read_page [4];
    uint8_t *write_page[4];
} mem_t;

int     mem_init       (mem_t *mem, const char *os, const char *basic, const char *amsdos);
void    mem_destroy    (mem_t *mem);
void    mem_update_map (mem_t *mem);
void    mem_select_rom (mem_t *mem, uint8_t rom_sel);

static inline uint8_t mem_read(mem_t *mem, uint16_t addr) {
    return mem->read_page[addr >> 14][addr & 0x3FFF];
}
static inline void mem_write(mem_t *mem, uint16_t addr, uint8_t val) {
    mem->write_page[addr >> 14][addr & 0x3FFF] = val;
}

#endif /* MEMORY_H */
