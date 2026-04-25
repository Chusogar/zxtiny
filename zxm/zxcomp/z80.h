/*
 * z80.h - Z80 CPU core interface
 *
 * Compatible with Carmikel's Z80 core:
 * https://github.com/carmikel/z80
 *
 * This header defines the Z80 state and interface functions.
 * The actual implementation comes from Carmikel's z80.c
 */

#ifndef Z80_H
#define Z80_H

#include <stdint.h>

/* ── Flags ─────────────────────────────────────────────────────────────── */
#define FLAG_C  0x01   /* Carry */
#define FLAG_N  0x02   /* Add/Subtract */
#define FLAG_PV 0x04   /* Parity/Overflow */
#define FLAG_3  0x08   /* undocumented */
#define FLAG_H  0x10   /* Half carry */
#define FLAG_5  0x20   /* undocumented */
#define FLAG_Z  0x40   /* Zero */
#define FLAG_S  0x80   /* Sign */

/* ── Register pair union ────────────────────────────────────────────────── */
typedef union {
    struct {
#ifdef __BIG_ENDIAN__
        uint8_t h, l;
#else
        uint8_t l, h;
#endif
    };
    uint16_t w;
} reg_pair_t;

/* ── Z80 CPU state ──────────────────────────────────────────────────────── */
typedef struct z80_s {
    /* Main registers */
    reg_pair_t AF, BC, DE, HL;
    /* Alternate registers */
    reg_pair_t AF_, BC_, DE_, HL_;
    /* Index registers */
    reg_pair_t IX, IY;
    /* Stack pointer and program counter */
    uint16_t SP, PC;
    /* Special registers */
    uint8_t  I, R;
    /* Interrupt flip-flops */
    uint8_t  IFF1, IFF2;
    /* Interrupt mode (0, 1 or 2) */
    uint8_t  IM;
    /* HALT state */
    uint8_t  halted;
    /* Pending NMI */
    uint8_t  nmi_pending;
    /* Pending INT */
    uint8_t  int_pending;
    /* T-states executed this call */
    int      cycles;

    /* ── Memory & I/O callbacks ─────────────────────────────────────── */
    void    *userdata;   /* passed to all callbacks */

    uint8_t (*mem_read )(struct z80_s *, uint16_t addr);
    void    (*mem_write)(struct z80_s *, uint16_t addr, uint8_t val);
    uint8_t (*io_read  )(struct z80_s *, uint16_t port);
    void    (*io_write )(struct z80_s *, uint16_t port, uint8_t val);
} z80_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Initialise CPU to power-on state */
void z80_init(z80_t *cpu);

/* Reset CPU (PC=0, IFF=0, IM=0, etc.) */
void z80_reset(z80_t *cpu);

/* Execute one instruction; returns T-states taken */
int  z80_step(z80_t *cpu);

/* Execute until at least `cycles` T-states have been consumed */
int  z80_run(z80_t *cpu, int cycles);

/* Raise / lower maskable interrupt line */
void z80_set_int(z80_t *cpu, int level);

/* Trigger non-maskable interrupt */
void z80_set_nmi(z80_t *cpu);

/* ── Convenience macros ──────────────────────────────────────────────────── */
#define Z80_A(c)  ((c)->AF.h)
#define Z80_F(c)  ((c)->AF.l)
#define Z80_B(c)  ((c)->BC.h)
#define Z80_C(c)  ((c)->BC.l)
#define Z80_D(c)  ((c)->DE.h)
#define Z80_E(c)  ((c)->DE.l)
#define Z80_H(c)  ((c)->HL.h)
#define Z80_L(c)  ((c)->HL.l)

#endif /* Z80_H */
