/*
 * sna.h / sna.c - Amstrad CPC SNA snapshot format
 *
 * SNA format (v1/v2/v3):
 *   Offset  Size  Content
 *   0x00    8     "MV - SNA"
 *   0x10    1     Version (1/2/3)
 *   0x11    2     Z80_AF
 *   0x13    2     Z80_BC
 *   0x15    2     Z80_DE
 *   0x17    2     Z80_HL
 *   0x19    2     Z80_R / Z80_I combined (R at low, I at high)
 *   0x1B    2     Z80_IY
 *   0x1D    2     Z80_IX
 *   0x1F    1     Z80_IFF (bit0=IFF1, bit1=IFF2)
 *   0x20    1     Z80_R (low 7 bits)
 *   0x21    2     Z80_AF'
 *   0x23    2     Z80_BC'
 *   0x25    2     Z80_DE'
 *   0x27    2     Z80_HL'
 *   0x29    2     Z80_SP
 *   0x2B    2     Z80_PC
 *   0x2D    1     Z80_IM (interrupt mode)
 *   0x2E    1     GA_PEN (gate array pen select)
 *   0x2F    17    GA_COLOUR (17 colour registers)
 *   0x40    1     GA_MULTI (Gate Array mode register)
 *   0x41    1     RAM_CFG (RAM banking)
 *   0x42    1     CRTC_SEL (selected CRTC register)
 *   0x43    18    CRTC_DATA (18 CRTC registers)
 *   0x55    1     ROM_CFG (upper ROM select)
 *   0x56    1     PPI_A
 *   0x57    1     PPI_B
 *   0x58    1     PPI_C
 *   0x59    1     PPI_CTRL
 *   0x5A    1     PSG_SEL
 *   0x5B    16    PSG_DATA
 *   0x6C    2     Memory dump size in KB (64 or 128)
 *   0x6E    ...   Memory dump
 */

#ifndef SNA_H
#define SNA_H

#include <stdint.h>

struct cpc_s;

int sna_load(struct cpc_s *cpc, const char *path);
int sna_save(struct cpc_s *cpc, const char *path);

#endif /* SNA_H */
