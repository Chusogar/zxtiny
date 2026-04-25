/*
 * ppi.c - Intel 8255 PPI
 */

#include "ppi.h"
#include "cpc.h"
#include "psg.h"
#include "keyboard.h"
#include <string.h>

void ppi_init(ppi_t *ppi)
{
    memset(ppi, 0, sizeof(*ppi));
    ppi->control = 0x82; /* Mode 0, Port A out, Port B in, Port C out */
    ppi->port_b  = 0xFF;
}

uint8_t ppi_read(ppi_t *ppi, cpc_t *cpc, uint8_t port)
{
    switch (port & 3) {
    case 0: /* Port A */
        if (ppi->control & 0x10) {
            /* Port A configured as input: read PSG data */
            return psg_read_data(&cpc->psg);
        }
        return ppi->port_a;

    case 1: /* Port B */
        {
            /* bit 7: cassette data in (1)
             * bit 6: printer busy (1=not busy)
             * bit 5: /EXP (expansion port)
             * bit 4: LK4 (50/60 Hz)  0=50Hz
             * bit 3: LK3
             * bit 2: LK2
             * bit 1: LK1
             * bit 0: VSYNC */
            uint8_t b = 0x7E; /* default: printer not busy, 50Hz */
            b |= cpc->crtc.vsync_active ? 0x01 : 0x00;

            /* Keyboard row read via Port C bits 3:0 */
            uint8_t row = ppi->port_c & 0x0F;
            b &= ~0x80; /* bit7 = keyboard data bit 7 (unused) */

            /* Read keyboard matrix */
            uint8_t key_data = keyboard_read_row(&cpc->keyboard, row);
            /* Port B bits 7..0 = keyboard row 0..7 */
            b = (b & ~0x80) | (key_data & 0x80 ? 0x80 : 0);

            return key_data;  /* simplified: return full keyboard row */
        }

    case 2: /* Port C */
        return ppi->port_c;

    default:
        return 0xFF;
    }
}

void ppi_write(ppi_t *ppi, cpc_t *cpc, uint8_t port, uint8_t val)
{
    switch (port & 3) {
    case 0: /* Port A */
        ppi->port_a = val;
        /* Port A drives PSG data bus */
        psg_write_data(&cpc->psg, val);
        break;

    case 1: /* Port B (input only in standard config) */
        break;

    case 2: /* Port C */
        ppi->port_c = val;
        {
            /* Bits 7:6 = PSG BDIR, BC1
             * BDIR=0,BC1=0: inactive
             * BDIR=0,BC1=1: read from PSG
             * BDIR=1,BC1=0: write to PSG
             * BDIR=1,BC1=1: latch address in PSG */
            uint8_t bdir = (val >> 7) & 1;
            uint8_t bc1  = (val >> 6) & 1;
            psg_control(&cpc->psg, bdir, bc1, ppi->port_a);

            /* Bits 3:0 = keyboard row select */
            /* (read happens via Port B) */

            /* Bit 4 = cassette motor */
            /* Bit 5 = cassette write */
        }
        break;

    case 3: /* Control word */
        ppi->control = val;
        if (val & 0x80) {
            /* Mode set */
            ppi->port_a = 0;
            ppi->port_c = 0;
        } else {
            /* Bit set/reset */
            uint8_t bit = (val >> 1) & 7;
            if (val & 1)
                ppi->port_c |=  (1 << bit);
            else
                ppi->port_c &= ~(1 << bit);
            /* Re-apply Port C effects */
            uint8_t bdir = (ppi->port_c >> 7) & 1;
            uint8_t bc1  = (ppi->port_c >> 6) & 1;
            psg_control(&cpc->psg, bdir, bc1, ppi->port_a);
        }
        break;
    }
}
