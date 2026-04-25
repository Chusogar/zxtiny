/*
 * ppi.h - Intel 8255 PPI (Programmable Peripheral Interface)
 *
 * Port A: PSG data bus
 * Port B: keyboard row input, VSYNC, printer busy
 * Port C: PSG BDIR/BC1, keyboard matrix select, cassette, loudspeaker
 */

#ifndef PPI_H
#define PPI_H

#include <stdint.h>

struct cpc_s;  /* forward */

typedef struct ppi_s {
    uint8_t port_a;    /* Port A output (PSG data bus) */
    uint8_t port_b;    /* Port B input */
    uint8_t port_c;    /* Port C output */
    uint8_t control;   /* Control word */
} ppi_t;

void    ppi_init (ppi_t *ppi);
uint8_t ppi_read (ppi_t *ppi, struct cpc_s *cpc, uint8_t port);
void    ppi_write(ppi_t *ppi, struct cpc_s *cpc, uint8_t port, uint8_t val);

#endif /* PPI_H */
