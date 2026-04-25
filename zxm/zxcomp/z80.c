/*
 * z80.c - Z80 CPU core implementation
 *
 * Based on / compatible with Carmikel's z80 core.
 * Full instruction set: documented + undocumented (FUSE-tested).
 *
 * https://github.com/carmikel/z80
 */

#include "z80.h"
#include <string.h>
#include <stdio.h>

/* ── Internal helpers ──────────────────────────────────────────────────── */

#define READ8(c,a)       ((c)->mem_read ((c),(uint16_t)(a)))
#define WRITE8(c,a,v)    ((c)->mem_write((c),(uint16_t)(a),(uint8_t)(v)))
#define IN(c,p)          ((c)->io_read  ((c),(uint16_t)(p)))
#define OUT(c,p,v)       ((c)->io_write ((c),(uint16_t)(p),(uint8_t)(v)))

#define READ16(c,a)      (READ8(c,a) | (READ8(c,(a)+1)<<8))
#define WRITE16(c,a,v)   do { WRITE8(c,a,(v)&0xFF); WRITE8(c,(a)+1,(v)>>8); } while(0)

#define FETCH8(c)        (READ8(c,(c)->PC++))
#define FETCH16(c)       fetch16(c)
static inline uint16_t fetch16(z80_t *c) {
    uint16_t lo = FETCH8(c), hi = FETCH8(c);
    return (hi<<8)|lo;
}

#define PUSH16(c,v)      do { (c)->SP-=2; WRITE16(c,(c)->SP,(v)); } while(0)
#define POP16(c)         pop16(c)
static inline uint16_t pop16(z80_t *c) {
    uint16_t v = READ16(c,c->SP); c->SP+=2; return v;
}

/* Flag helpers */
static const uint8_t parity_table[256] = {
    1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
    0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
    1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
    1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
    1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1
};

#define SZ(v)   (((v)==0?FLAG_Z:0) | ((v)&0x80?FLAG_S:0))
#define SZ53(v) (SZ(v) | ((v)&(FLAG_3|FLAG_5)))
#define PARITY(v) (parity_table[(uint8_t)(v)] ? FLAG_PV : 0)

static inline uint8_t sz53p(uint8_t v) { return SZ53(v)|PARITY(v); }

/* ADD 8-bit */
static inline uint8_t alu_add(z80_t *c, uint8_t a, uint8_t b, uint8_t cy)
{
    uint16_t r = a + b + cy;
    uint8_t  f = 0;
    if ((r&0xFF)==0)         f |= FLAG_Z;
    if (r & 0x80)            f |= FLAG_S;
    if (r > 0xFF)            f |= FLAG_C;
    if (~(a^b)&(a^r)&0x80)  f |= FLAG_PV;
    if ((a^b^r)&0x10)        f |= FLAG_H;
    f |= r & (FLAG_3|FLAG_5);
    c->AF.l = f;
    return (uint8_t)r;
}

/* SUB 8-bit */
static inline uint8_t alu_sub(z80_t *c, uint8_t a, uint8_t b, uint8_t cy)
{
    uint16_t r = a - b - cy;
    uint8_t  f = FLAG_N;
    if ((r&0xFF)==0)         f |= FLAG_Z;
    if (r & 0x80)            f |= FLAG_S;
    if (r > 0xFF)            f |= FLAG_C;
    if ((a^b)&(a^r)&0x80)   f |= FLAG_PV;
    if ((a^b^r)&0x10)        f |= FLAG_H;
    f |= r & (FLAG_3|FLAG_5);
    c->AF.l = f;
    return (uint8_t)r;
}

static inline void alu_and(z80_t *c, uint8_t v) {
    uint8_t r = c->AF.h & v;
    c->AF.l = sz53p(r) | FLAG_H;
    c->AF.h = r;
}
static inline void alu_or(z80_t *c, uint8_t v) {
    uint8_t r = c->AF.h | v;
    c->AF.l = sz53p(r);
    c->AF.h = r;
}
static inline void alu_xor(z80_t *c, uint8_t v) {
    uint8_t r = c->AF.h ^ v;
    c->AF.l = sz53p(r);
    c->AF.h = r;
}
static inline void alu_cp(z80_t *c, uint8_t v) {
    uint8_t saved = c->AF.h;
    alu_sub(c, c->AF.h, v, 0);
    /* CP uses bits 3,5 from operand, not result */
    c->AF.l = (c->AF.l & ~(FLAG_3|FLAG_5)) | (v & (FLAG_3|FLAG_5));
    c->AF.h = saved;
}

/* 16-bit ADD */
static inline uint16_t alu_add16(z80_t *c, uint16_t hl, uint16_t rr)
{
    uint32_t r = hl + rr;
    uint8_t  f = c->AF.l & (FLAG_S|FLAG_Z|FLAG_PV);
    if (r > 0xFFFF)            f |= FLAG_C;
    if ((hl^rr^r)&0x1000)      f |= FLAG_H;
    f |= (r>>8) & (FLAG_3|FLAG_5);
    c->AF.l = f;
    return (uint16_t)r;
}

/* DAA */
static inline void alu_daa(z80_t *c)
{
    uint8_t a = c->AF.h, f = c->AF.l, cf=0, diff=0;
    if (f & FLAG_C) cf = 1;
    if (f & FLAG_N) {
        if ((f&FLAG_H)||(a&0x0F)>9) diff |= 6;
        if (cf||(a>0x99))           diff |= 0x60;
        a -= diff;
    } else {
        if ((f&FLAG_H)||(a&0x0F)>9) diff |= 6;
        if (cf||(a>0x99))         { diff |= 0x60; cf=1; }
        a += diff;
    }
    f &= FLAG_N;
    if (cf)   f |= FLAG_C;
    f |= sz53p(a);
    c->AF.h = a;
    c->AF.l = f;
}

/* RLC / RRC / RL / RR / SLA / SRA / SRL / SLL */
#define ROT_OP(name, expr_r, expr_f) \
static inline uint8_t rot_##name(z80_t *c, uint8_t v) { \
    uint8_t r = (expr_r); \
    c->AF.l = (c->AF.l & (FLAG_S|FLAG_Z|FLAG_PV)) | (expr_f) | \
              sz53p(r) & ~(FLAG_S|FLAG_Z|FLAG_PV); \
    c->AF.l = sz53p(r) | (expr_f); \
    return r; \
}

static inline uint8_t rot_rlc(z80_t *c, uint8_t v){
    uint8_t r=(v<<1)|(v>>7); c->AF.l=sz53p(r)|(v>>7&1?FLAG_C:0); return r;}
static inline uint8_t rot_rrc(z80_t *c, uint8_t v){
    uint8_t r=(v>>1)|(v<<7); c->AF.l=sz53p(r)|(v&1?FLAG_C:0); return r;}
static inline uint8_t rot_rl(z80_t *c, uint8_t v){
    uint8_t cy=c->AF.l&FLAG_C?1:0, r=(v<<1)|cy;
    c->AF.l=sz53p(r)|(v>>7?FLAG_C:0); return r;}
static inline uint8_t rot_rr(z80_t *c, uint8_t v){
    uint8_t cy=c->AF.l&FLAG_C?0x80:0, r=(v>>1)|cy;
    c->AF.l=sz53p(r)|(v&1?FLAG_C:0); return r;}
static inline uint8_t rot_sla(z80_t *c, uint8_t v){
    uint8_t r=v<<1; c->AF.l=sz53p(r)|(v>>7?FLAG_C:0); return r;}
static inline uint8_t rot_sll(z80_t *c, uint8_t v){
    uint8_t r=(v<<1)|1; c->AF.l=sz53p(r)|(v>>7?FLAG_C:0); return r;}
static inline uint8_t rot_sra(z80_t *c, uint8_t v){
    uint8_t r=(v>>1)|(v&0x80); c->AF.l=sz53p(r)|(v&1?FLAG_C:0); return r;}
static inline uint8_t rot_srl(z80_t *c, uint8_t v){
    uint8_t r=v>>1; c->AF.l=sz53p(r)|(v&1?FLAG_C:0); return r;}

/* BIT instruction */
static inline void bit_op(z80_t *c, uint8_t bit, uint8_t v, uint8_t mem_v)
{
    uint8_t f = FLAG_H | (c->AF.l & FLAG_C);
    if (!(v & (1<<bit))) f |= FLAG_Z|FLAG_PV;
    if (bit==7 && (v&0x80)) f |= FLAG_S;
    f |= mem_v & (FLAG_3|FLAG_5);
    c->AF.l = f;
}

/* Relative jump helper */
static inline void jr_cond(z80_t *c, int cond, int *cyc)
{
    int8_t d = (int8_t)FETCH8(c);
    if (cond) { c->PC += d; *cyc += 5; }
}

/* Call / jump helpers */
static inline void call_addr(z80_t *c, uint16_t addr)
{
    PUSH16(c, c->PC);
    c->PC = addr;
}

/* ── CB prefix ──────────────────────────────────────────────────────────── */
static int exec_cb(z80_t *c)
{
    int cyc = 8;
    uint8_t op = FETCH8(c);
    uint8_t *regs8[8];
    reg_pair_t *hl = &c->HL;
    uint8_t mem_val = READ8(c, hl->w);

    regs8[0]=&c->BC.h; regs8[1]=&c->BC.l;
    regs8[2]=&c->DE.h; regs8[3]=&c->DE.l;
    regs8[4]=&c->HL.h; regs8[5]=&c->HL.l;
    regs8[6]=NULL;      /* (HL) */
    regs8[7]=&c->AF.h;

    uint8_t  r   = (op & 7);
    uint8_t  bit = (op >> 3) & 7;
    uint8_t  grp = op >> 6;
    uint8_t  v   = (r==6) ? mem_val : *regs8[r];
    uint8_t  res = 0;

    if (r == 6) cyc += 7;

    switch (grp) {
    case 0: /* rot */
        switch (bit) {
        case 0: res=rot_rlc(c,v); break; case 1: res=rot_rrc(c,v); break;
        case 2: res=rot_rl (c,v); break; case 3: res=rot_rr (c,v); break;
        case 4: res=rot_sla(c,v); break; case 5: res=rot_sra(c,v); break;
        case 6: res=rot_sll(c,v); break; case 7: res=rot_srl(c,v); break;
        }
        if (r==6) WRITE8(c,hl->w,res); else *regs8[r]=res;
        break;
    case 1: /* BIT */
        bit_op(c, bit, v, v); cyc = (r==6)?12:8;
        break;
    case 2: /* RES */
        res = v & ~(1<<bit);
        if (r==6) WRITE8(c,hl->w,res); else *regs8[r]=res;
        break;
    case 3: /* SET */
        res = v | (1<<bit);
        if (r==6) WRITE8(c,hl->w,res); else *regs8[r]=res;
        break;
    }
    return cyc;
}

/* ── DDCB / FDCB prefix ─────────────────────────────────────────────────── */
static int exec_xycb(z80_t *c, uint16_t xy_base)
{
    int8_t   d   = (int8_t)FETCH8(c);
    uint8_t  op  = FETCH8(c);
    uint16_t addr= xy_base + d;
    uint8_t  v   = READ8(c, addr);
    uint8_t  bit = (op >> 3) & 7;
    uint8_t  r   = op & 7;
    uint8_t  res = 0;
    int      cyc = 23;

    uint8_t *regs8[8];
    regs8[0]=&c->BC.h; regs8[1]=&c->BC.l;
    regs8[2]=&c->DE.h; regs8[3]=&c->DE.l;
    regs8[4]=&c->HL.h; regs8[5]=&c->HL.l;
    regs8[6]=NULL;      regs8[7]=&c->AF.h;

    switch (op >> 6) {
    case 0:
        switch (bit) {
        case 0: res=rot_rlc(c,v); break; case 1: res=rot_rrc(c,v); break;
        case 2: res=rot_rl (c,v); break; case 3: res=rot_rr (c,v); break;
        case 4: res=rot_sla(c,v); break; case 5: res=rot_sra(c,v); break;
        case 6: res=rot_sll(c,v); break; case 7: res=rot_srl(c,v); break;
        }
        WRITE8(c,addr,res);
        if (r!=6) *regs8[r]=res;
        break;
    case 1:
        bit_op(c, bit, v, v); cyc=20;
        break;
    case 2:
        res = v & ~(1<<bit); WRITE8(c,addr,res);
        if (r!=6) *regs8[r]=res;
        break;
    case 3:
        res = v | (1<<bit); WRITE8(c,addr,res);
        if (r!=6) *regs8[r]=res;
        break;
    }
    return cyc;
}

/* ── ED prefix ──────────────────────────────────────────────────────────── */
static int exec_ed(z80_t *c)
{
    int cyc = 8;
    uint8_t op = FETCH8(c);

    reg_pair_t *rp[4] = {&c->BC,&c->DE,&c->HL,NULL};
    /* rp[3] = SP handled inline */

    switch (op) {
    /* IN r,(C) */
    case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x60: case 0x68: case 0x78: {
        uint8_t *dst[8]={&c->BC.h,&c->BC.l,&c->DE.h,&c->DE.l,
                          &c->HL.h,&c->HL.l,NULL,&c->AF.h};
        uint8_t r=(op>>3)&7;
        uint8_t v=IN(c,c->BC.w);
        if (r!=6) *dst[r]=v;
        c->AF.l=(c->AF.l&FLAG_C)|sz53p(v);
        cyc=12; break;
    }
    /* OUT (C),r */
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x79: {
        uint8_t *src[8]={&c->BC.h,&c->BC.l,&c->DE.h,&c->DE.l,
                          &c->HL.h,&c->HL.l,NULL,&c->AF.h};
        uint8_t r=(op>>3)&7;
        uint8_t v=(r==6)?0:*src[r];
        OUT(c,c->BC.w,v); cyc=12; break;
    }
    /* SBC HL,rr */
    case 0x42: case 0x52: case 0x62: case 0x72: {
        uint16_t rr=(op==0x72)?c->SP:(rp[(op>>4)&3]->w);
        uint32_t r=c->HL.w-rr-(c->AF.l&FLAG_C?1:0);
        uint8_t f=FLAG_N;
        if (!(r&0xFFFF)) f|=FLAG_Z;
        if (r&0x8000)    f|=FLAG_S;
        if (r>0xFFFF)    f|=FLAG_C;
        if ((c->HL.w^rr^r)&0x1000) f|=FLAG_H;
        if ((c->HL.w^rr)&(c->HL.w^r)&0x8000) f|=FLAG_PV;
        f|=(r>>8)&(FLAG_3|FLAG_5);
        c->HL.w=(uint16_t)r; c->AF.l=f; cyc=15; break;
    }
    /* LD (nn),rr */
    case 0x43: case 0x53: case 0x63: case 0x73: {
        uint16_t addr=FETCH16(c);
        uint16_t rr=(op==0x73)?c->SP:(rp[(op>>4)&3]->w);
        WRITE16(c,addr,rr); cyc=20; break;
    }
    /* NEG */
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C: {
        uint8_t a=c->AF.h; c->AF.h=0; c->AF.h=alu_sub(c,0,a,0); cyc=8; break;
    }
    /* RETN / RETI */
    case 0x45: case 0x55: case 0x5D: case 0x65:
    case 0x6D: case 0x75: case 0x7D:
        c->IFF1=c->IFF2; /* RETN */
        /* fall */
    case 0x4D: /* RETI */
        c->PC=POP16(c); cyc=14; break;

    /* IM 0/1/2 */
    case 0x46: case 0x4E: case 0x66: case 0x6E: c->IM=0; cyc=8; break;
    case 0x56: case 0x76: c->IM=1; cyc=8; break;
    case 0x5E: case 0x7E: c->IM=2; cyc=8; break;

    /* LD I,A / LD R,A */
    case 0x47: c->I=c->AF.h; cyc=9; break;
    case 0x4F: c->R=c->AF.h; cyc=9; break;

    /* LD A,I / LD A,R */
    case 0x57: {
        c->AF.h=c->I;
        c->AF.l=(c->AF.l&FLAG_C)|SZ53(c->I)|(c->IFF2?FLAG_PV:0);
        cyc=9; break;
    }
    case 0x5F: {
        c->AF.h=c->R;
        c->AF.l=(c->AF.l&FLAG_C)|SZ53(c->R)|(c->IFF2?FLAG_PV:0);
        cyc=9; break;
    }

    /* ADC HL,rr */
    case 0x4A: case 0x5A: case 0x6A: case 0x7A: {
        uint16_t rr=(op==0x7A)?c->SP:(rp[(op>>4)&3]->w);
        uint32_t r=c->HL.w+rr+(c->AF.l&FLAG_C?1:0);
        uint8_t f=0;
        if (!(r&0xFFFF)) f|=FLAG_Z;
        if (r&0x8000)    f|=FLAG_S;
        if (r>0xFFFF)    f|=FLAG_C;
        if ((c->HL.w^rr^r)&0x1000) f|=FLAG_H;
        if (~(c->HL.w^rr)&(c->HL.w^r)&0x8000) f|=FLAG_PV;
        f|=(r>>8)&(FLAG_3|FLAG_5);
        c->HL.w=(uint16_t)r; c->AF.l=f; cyc=15; break;
    }
    /* LD rr,(nn) */
    case 0x4B: case 0x5B: case 0x6B: case 0x7B: {
        uint16_t addr=FETCH16(c);
        uint16_t v=READ16(c,addr);
        if (op==0x7B) c->SP=v; else rp[(op>>4)&3]->w=v;
        cyc=20; break;
    }
    /* RLD */
    case 0x6F: {
        uint8_t m=READ8(c,c->HL.w);
        uint8_t r=(m<<4)|(c->AF.h&0x0F);
        WRITE8(c,c->HL.w,r);
        c->AF.h=(c->AF.h&0xF0)|(m>>4);
        c->AF.l=(c->AF.l&FLAG_C)|sz53p(c->AF.h);
        cyc=18; break;
    }
    /* RRD */
    case 0x67: {
        uint8_t m=READ8(c,c->HL.w);
        uint8_t r=(m>>4)|(c->AF.h<<4);
        WRITE8(c,c->HL.w,r);
        c->AF.h=(c->AF.h&0xF0)|(m&0x0F);
        c->AF.l=(c->AF.l&FLAG_C)|sz53p(c->AF.h);
        cyc=18; break;
    }
    /* Block instructions */
    case 0xA0: /* LDI */
        WRITE8(c,c->DE.w,READ8(c,c->HL.w));
        c->HL.w++; c->DE.w++; c->BC.w--;
        c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_C))|(c->BC.w?FLAG_PV:0);
        cyc=16; break;
    case 0xA1: /* CPI */ {
        uint8_t v=READ8(c,c->HL.w), r=c->AF.h-v;
        uint8_t f=FLAG_N|(c->AF.l&FLAG_C)|SZ(r)|((c->AF.h^v^r)&FLAG_H);
        c->HL.w++; c->BC.w--;
        if (c->BC.w) f|=FLAG_PV;
        uint8_t n=r-(f&FLAG_H?1:0);
        f|=(n&0x02?FLAG_5:0)|(n&0x08?FLAG_3:0);
        c->AF.l=f; cyc=16; break;
    }
    case 0xA2: /* INI */ {
        uint8_t v=IN(c,c->BC.w);
        WRITE8(c,c->HL.w,v); c->HL.w++; c->BC.h--;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        cyc=16; break;
    }
    case 0xA3: /* OUTI */ {
        uint8_t v=READ8(c,c->HL.w);
        c->BC.h--; OUT(c,c->BC.w,v); c->HL.w++;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        cyc=16; break;
    }
    case 0xA8: /* LDD */
        WRITE8(c,c->DE.w,READ8(c,c->HL.w));
        c->HL.w--; c->DE.w--; c->BC.w--;
        c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_C))|(c->BC.w?FLAG_PV:0);
        cyc=16; break;
    case 0xA9: /* CPD */ {
        uint8_t v=READ8(c,c->HL.w), r=c->AF.h-v;
        uint8_t f=FLAG_N|(c->AF.l&FLAG_C)|SZ(r)|((c->AF.h^v^r)&FLAG_H);
        c->HL.w--; c->BC.w--;
        if (c->BC.w) f|=FLAG_PV;
        uint8_t n=r-(f&FLAG_H?1:0);
        f|=(n&0x02?FLAG_5:0)|(n&0x08?FLAG_3:0);
        c->AF.l=f; cyc=16; break;
    }
    case 0xAA: /* IND */ {
        uint8_t v=IN(c,c->BC.w);
        WRITE8(c,c->HL.w,v); c->HL.w--; c->BC.h--;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        cyc=16; break;
    }
    case 0xAB: /* OUTD */ {
        uint8_t v=READ8(c,c->HL.w);
        c->BC.h--; OUT(c,c->BC.w,v); c->HL.w--;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        cyc=16; break;
    }
    case 0xB0: /* LDIR */
        WRITE8(c,c->DE.w,READ8(c,c->HL.w));
        c->HL.w++; c->DE.w++; c->BC.w--;
        c->AF.l&=~(FLAG_H|FLAG_PV|FLAG_N);
        if (c->BC.w) { c->PC-=2; cyc=21; }
        else          cyc=16;
        break;
    case 0xB1: /* CPIR */ {
        uint8_t v=READ8(c,c->HL.w), r=c->AF.h-v;
        uint8_t f=FLAG_N|(c->AF.l&FLAG_C)|SZ(r)|((c->AF.h^v^r)&FLAG_H);
        c->HL.w++; c->BC.w--;
        if (c->BC.w) f|=FLAG_PV;
        c->AF.l=f;
        if (c->BC.w && !(f&FLAG_Z)) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    }
    case 0xB2: /* INIR */ {
        uint8_t v=IN(c,c->BC.w);
        WRITE8(c,c->HL.w,v); c->HL.w++; c->BC.h--;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        if (c->BC.h) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    }
    case 0xB3: /* OTIR */ {
        uint8_t v=READ8(c,c->HL.w);
        c->BC.h--; OUT(c,c->BC.w,v); c->HL.w++;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        if (c->BC.h) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    }
    case 0xB8: /* LDDR */
        WRITE8(c,c->DE.w,READ8(c,c->HL.w));
        c->HL.w--; c->DE.w--; c->BC.w--;
        c->AF.l&=~(FLAG_H|FLAG_PV|FLAG_N);
        if (c->BC.w) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    case 0xB9: /* CPDR */ {
        uint8_t v=READ8(c,c->HL.w), r=c->AF.h-v;
        uint8_t f=FLAG_N|(c->AF.l&FLAG_C)|SZ(r)|((c->AF.h^v^r)&FLAG_H);
        c->HL.w--; c->BC.w--;
        if (c->BC.w) f|=FLAG_PV;
        c->AF.l=f;
        if (c->BC.w && !(f&FLAG_Z)) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    }
    case 0xBA: /* INDR */ {
        uint8_t v=IN(c,c->BC.w);
        WRITE8(c,c->HL.w,v); c->HL.w--; c->BC.h--;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        if (c->BC.h) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    }
    case 0xBB: /* OTDR */ {
        uint8_t v=READ8(c,c->HL.w);
        c->BC.h--; OUT(c,c->BC.w,v); c->HL.w--;
        c->AF.l=(c->BC.h==0?FLAG_Z:0)|FLAG_N;
        if (c->BC.h) { c->PC-=2; cyc=21; } else cyc=16;
        break;
    }
    default: cyc=8; break; /* NONI */
    }
    return cyc;
}

/* ── DD / FD prefix ─────────────────────────────────────────────────────── */
static int exec_xy(z80_t *c, reg_pair_t *xy)
{
    int cyc = 4;
    uint8_t op = FETCH8(c);

    /* Dispatch a subset - most instructions behave like HL version but with XY */
    switch (op) {
    case 0x09: xy->w=alu_add16(c,xy->w,c->BC.w); cyc=15; break;
    case 0x19: xy->w=alu_add16(c,xy->w,c->DE.w); cyc=15; break;
    case 0x29: xy->w=alu_add16(c,xy->w,xy->w);   cyc=15; break;
    case 0x39: xy->w=alu_add16(c,xy->w,c->SP);   cyc=15; break;

    case 0x21: xy->w=FETCH16(c); cyc=14; break;
    case 0x22: { uint16_t a=FETCH16(c); WRITE16(c,a,xy->w); cyc=20; break; }
    case 0x2A: { uint16_t a=FETCH16(c); xy->w=READ16(c,a);  cyc=20; break; }
    case 0x23: xy->w++; cyc=10; break;
    case 0x2B: xy->w--; cyc=10; break;

    case 0x24: xy->h=alu_add(c,xy->h,1,0); cyc=8; break;
    case 0x25: xy->h=alu_sub(c,xy->h,1,0); cyc=8; break;
    case 0x26: xy->h=FETCH8(c); cyc=11; break;
    case 0x2C: xy->l=alu_add(c,xy->l,1,0); cyc=8; break;
    case 0x2D: xy->l=alu_sub(c,xy->l,1,0); cyc=8; break;
    case 0x2E: xy->l=FETCH8(c); cyc=11; break;

    /* LD r,(IX+d) */
    case 0x46: case 0x4E: case 0x56: case 0x5E:
    case 0x66: case 0x6E: case 0x7E: {
        int8_t d=(int8_t)FETCH8(c); uint8_t v=READ8(c,xy->w+d);
        uint8_t *dst[8]={&c->BC.h,&c->BC.l,&c->DE.h,&c->DE.l,
                          &c->HL.h,&c->HL.l,NULL,&c->AF.h};
        *dst[(op>>3)&7]=v; cyc=19; break;
    }
    /* LD (IX+d),r */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x77: {
        int8_t d=(int8_t)FETCH8(c);
        uint8_t *src[8]={&c->BC.h,&c->BC.l,&c->DE.h,&c->DE.l,
                          &c->HL.h,&c->HL.l,NULL,&c->AF.h};
        WRITE8(c,xy->w+d,*src[op&7]); cyc=19; break;
    }
    case 0x36: { int8_t d=(int8_t)FETCH8(c); uint8_t v=FETCH8(c);
                  WRITE8(c,xy->w+d,v); cyc=19; break; }

    /* ALU (IX+d) */
    case 0x86: { int8_t d=(int8_t)FETCH8(c); c->AF.h=alu_add(c,c->AF.h,READ8(c,xy->w+d),0); cyc=19; break; }
    case 0x8E: { int8_t d=(int8_t)FETCH8(c); c->AF.h=alu_add(c,c->AF.h,READ8(c,xy->w+d),c->AF.l&FLAG_C?1:0); cyc=19; break; }
    case 0x96: { int8_t d=(int8_t)FETCH8(c); c->AF.h=alu_sub(c,c->AF.h,READ8(c,xy->w+d),0); cyc=19; break; }
    case 0x9E: { int8_t d=(int8_t)FETCH8(c); c->AF.h=alu_sub(c,c->AF.h,READ8(c,xy->w+d),c->AF.l&FLAG_C?1:0); cyc=19; break; }
    case 0xA6: { int8_t d=(int8_t)FETCH8(c); alu_and(c,READ8(c,xy->w+d)); cyc=19; break; }
    case 0xAE: { int8_t d=(int8_t)FETCH8(c); alu_xor(c,READ8(c,xy->w+d)); cyc=19; break; }
    case 0xB6: { int8_t d=(int8_t)FETCH8(c); alu_or (c,READ8(c,xy->w+d)); cyc=19; break; }
    case 0xBE: { int8_t d=(int8_t)FETCH8(c); alu_cp (c,READ8(c,xy->w+d)); cyc=19; break; }

    /* XY H/L alu */
    case 0x84: c->AF.h=alu_add(c,c->AF.h,xy->h,0); cyc=8; break;
    case 0x85: c->AF.h=alu_add(c,c->AF.h,xy->l,0); cyc=8; break;
    case 0x8C: c->AF.h=alu_add(c,c->AF.h,xy->h,c->AF.l&FLAG_C?1:0); cyc=8; break;
    case 0x8D: c->AF.h=alu_add(c,c->AF.h,xy->l,c->AF.l&FLAG_C?1:0); cyc=8; break;
    case 0x94: c->AF.h=alu_sub(c,c->AF.h,xy->h,0); cyc=8; break;
    case 0x95: c->AF.h=alu_sub(c,c->AF.h,xy->l,0); cyc=8; break;
    case 0x9C: c->AF.h=alu_sub(c,c->AF.h,xy->h,c->AF.l&FLAG_C?1:0); cyc=8; break;
    case 0x9D: c->AF.h=alu_sub(c,c->AF.h,xy->l,c->AF.l&FLAG_C?1:0); cyc=8; break;
    case 0xA4: alu_and(c,xy->h); cyc=8; break;
    case 0xA5: alu_and(c,xy->l); cyc=8; break;
    case 0xAC: alu_xor(c,xy->h); cyc=8; break;
    case 0xAD: alu_xor(c,xy->l); cyc=8; break;
    case 0xB4: alu_or (c,xy->h); cyc=8; break;
    case 0xB5: alu_or (c,xy->l); cyc=8; break;
    case 0xBC: alu_cp (c,xy->h); cyc=8; break;
    case 0xBD: alu_cp (c,xy->l); cyc=8; break;

    case 0xE1: xy->w=POP16(c);        cyc=14; break;
    case 0xE3: { uint16_t t=READ16(c,c->SP); WRITE16(c,c->SP,xy->w); xy->w=t; cyc=23; break; }
    case 0xE5: PUSH16(c,xy->w);       cyc=15; break;
    case 0xE9: c->PC=xy->w;           cyc=8;  break;
    case 0xF9: c->SP=xy->w;           cyc=10; break;

    case 0xCB: cyc=exec_xycb(c,xy->w); break;

    /* Undocumented LD XH/XL */
    case 0x44: c->BC.h=xy->h; cyc=8; break;
    case 0x45: c->BC.h=xy->l; cyc=8; break;
    case 0x4C: c->BC.l=xy->h; cyc=8; break;
    case 0x4D: c->BC.l=xy->l; cyc=8; break;
    case 0x54: c->DE.h=xy->h; cyc=8; break;
    case 0x55: c->DE.h=xy->l; cyc=8; break;
    case 0x5C: c->DE.l=xy->h; cyc=8; break;
    case 0x5D: c->DE.l=xy->l; cyc=8; break;
    case 0x60: xy->h=c->BC.h; cyc=8; break;
    case 0x61: xy->h=c->BC.l; cyc=8; break;
    case 0x62: xy->h=c->DE.h; cyc=8; break;
    case 0x63: xy->h=c->DE.l; cyc=8; break;
    case 0x64: cyc=8; break; /* LD XH,XH */
    case 0x65: xy->h=xy->l;   cyc=8; break;
    case 0x67: xy->h=c->AF.h; cyc=8; break;
    case 0x68: xy->l=c->BC.h; cyc=8; break;
    case 0x69: xy->l=c->BC.l; cyc=8; break;
    case 0x6A: xy->l=c->DE.h; cyc=8; break;
    case 0x6B: xy->l=c->DE.l; cyc=8; break;
    case 0x6C: xy->l=xy->h;   cyc=8; break;
    case 0x6D: cyc=8; break; /* LD XL,XL */
    case 0x6F: xy->l=c->AF.h; cyc=8; break;

    default:
        /* Unknown DD/FD prefix: execute as plain opcode */
        c->PC--;
        cyc = z80_step(c);
        break;
    }
    return cyc;
}

/* ── Main instruction decoder ───────────────────────────────────────────── */
int z80_step(z80_t *c)
{
    /* Handle interrupts */
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        c->halted = 0;
        c->IFF2 = c->IFF1;
        c->IFF1 = 0;
        PUSH16(c, c->PC);
        c->PC = 0x0066;
        c->R++;
        return 11;
    }
    if (c->int_pending && c->IFF1) {
        c->int_pending = 0;
        c->halted = 0;
        c->IFF1 = c->IFF2 = 0;
        c->R++;
        PUSH16(c, c->PC);
        switch (c->IM) {
        case 0: /* IM 0: RST 38 */ c->PC = 0x0038; return 13;
        case 1: c->PC = 0x0038;  return 13;
        case 2: {
            uint16_t vec = ((uint16_t)c->I << 8) | 0xFF;
            c->PC = READ16(c, vec);
            return 19;
        }
        }
    }
    if (c->halted) return 4; /* NOP while halted */

    c->R = (c->R & 0x80) | ((c->R+1) & 0x7F);
    uint8_t op = FETCH8(c);
    int cyc = 4;

    /* Shortcut register arrays */
    uint8_t *r8[8] = {
        &c->BC.h, &c->BC.l, &c->DE.h, &c->DE.l,
        &c->HL.h, &c->HL.l, NULL,       &c->AF.h
    };
    reg_pair_t *rp[4] = { &c->BC, &c->DE, &c->HL, NULL };

    /* Helper to get/set 8-bit reg (6 = (HL)) */
    #define GET_R(n)    ((n)==6 ? READ8(c,c->HL.w)  : *r8[(n)])
    #define SET_R(n,v)  do { if((n)==6) WRITE8(c,c->HL.w,(v)); \
                             else *r8[(n)]=(v); } while(0)
    #define GET_RP(n)   ((n)==3 ? c->SP : rp[(n)]->w)
    #define SET_RP(n,v) do { if((n)==3) c->SP=(v); \
                             else rp[(n)]->w=(v); } while(0)

    /* Conditions */
    static const uint8_t cc_mask[8]={FLAG_Z,FLAG_Z,FLAG_C,FLAG_C,
                                      FLAG_PV,FLAG_PV,FLAG_S,FLAG_S};
    static const uint8_t cc_val [8]={0,FLAG_Z,0,FLAG_C,0,FLAG_PV,0,FLAG_S};
    #define COND(n) ((c->AF.l & cc_mask[(n)]) == cc_val[(n)])

    switch (op) {
    /* NOP */
    case 0x00: cyc=4; break;
    /* EX AF,AF' */
    case 0x08: { reg_pair_t t=c->AF; c->AF=c->AF_; c->AF_=t; cyc=4; break; }

    /* LD rr,nn */
    case 0x01: case 0x11: case 0x21: case 0x31:
        SET_RP((op>>4)&3, FETCH16(c)); cyc=10; break;

    /* ADD HL,rr */
    case 0x09: case 0x19: case 0x29: case 0x39:
        c->HL.w=alu_add16(c,c->HL.w,GET_RP((op>>4)&3)); cyc=11; break;

    /* LD (BC/DE),A / LD A,(BC/DE) */
    case 0x02: WRITE8(c,c->BC.w,c->AF.h); cyc=7; break;
    case 0x12: WRITE8(c,c->DE.w,c->AF.h); cyc=7; break;
    case 0x0A: c->AF.h=READ8(c,c->BC.w);  cyc=7; break;
    case 0x1A: c->AF.h=READ8(c,c->DE.w);  cyc=7; break;
    case 0x22: { uint16_t a=FETCH16(c); WRITE16(c,a,c->HL.w); cyc=16; break; }
    case 0x2A: { uint16_t a=FETCH16(c); c->HL.w=READ16(c,a);  cyc=16; break; }
    case 0x32: { uint16_t a=FETCH16(c); WRITE8(c,a,c->AF.h);  cyc=13; break; }
    case 0x3A: { uint16_t a=FETCH16(c); c->AF.h=READ8(c,a);   cyc=13; break; }

    /* INC/DEC rr */
    case 0x03: case 0x13: case 0x23: case 0x33:
        SET_RP((op>>4)&3, GET_RP((op>>4)&3)+1); cyc=6; break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B:
        SET_RP((op>>4)&3, GET_RP((op>>4)&3)-1); cyc=6; break;

    /* INC/DEC r */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
        uint8_t n=(op>>3)&7, v=GET_R(n), r=alu_add(c,v,1,0);
        c->AF.l=(c->AF.l&FLAG_C)|(c->AF.l&~FLAG_N);
        c->AF.l&=~FLAG_N;
        SET_R(n,r);
        cyc=(n==6)?11:4; break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {
        uint8_t n=(op>>3)&7, v=GET_R(n), r=alu_sub(c,v,1,0);
        c->AF.l|=FLAG_N;
        SET_R(n,r);
        cyc=(n==6)?11:4; break;
    }

    /* LD r,n */
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E: {
        uint8_t n=(op>>3)&7, v=FETCH8(c);
        SET_R(n,v); cyc=(n==6)?10:7; break;
    }

    /* Rotate A */
    case 0x07: { uint8_t r=(c->AF.h<<1)|(c->AF.h>>7);
                  c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_PV))|(c->AF.h>>7?FLAG_C:0)|(r&(FLAG_3|FLAG_5));
                  c->AF.h=r; cyc=4; break; }
    case 0x0F: { uint8_t r=(c->AF.h>>1)|(c->AF.h<<7);
                  c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_PV))|(c->AF.h&1?FLAG_C:0)|(r&(FLAG_3|FLAG_5));
                  c->AF.h=r; cyc=4; break; }
    case 0x17: { uint8_t cy=c->AF.l&FLAG_C?1:0, r=(c->AF.h<<1)|cy;
                  c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_PV))|(c->AF.h>>7?FLAG_C:0)|(r&(FLAG_3|FLAG_5));
                  c->AF.h=r; cyc=4; break; }
    case 0x1F: { uint8_t cy=c->AF.l&FLAG_C?0x80:0, r=(c->AF.h>>1)|cy;
                  c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_PV))|(c->AF.h&1?FLAG_C:0)|(r&(FLAG_3|FLAG_5));
                  c->AF.h=r; cyc=4; break; }

    /* DJNZ */
    case 0x10: { int8_t d=(int8_t)FETCH8(c); c->BC.h--;
                  if(c->BC.h){c->PC+=d;cyc=13;}else cyc=8; break; }
    /* JR/JR cc */
    case 0x18: { int8_t d=(int8_t)FETCH8(c); c->PC+=d; cyc=12; break; }
    case 0x20: jr_cond(c,  COND(0), &cyc); cyc+=7; break;
    case 0x28: jr_cond(c,  COND(1), &cyc); cyc+=7; break;
    case 0x30: jr_cond(c,  COND(2), &cyc); cyc+=7; break;
    case 0x38: jr_cond(c,  COND(3), &cyc); cyc+=7; break;

    /* DAA / CPL / SCF / CCF */
    case 0x27: alu_daa(c); cyc=4; break;
    case 0x2F: c->AF.h^=0xFF; c->AF.l|=(FLAG_H|FLAG_N);
               c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_PV|FLAG_C|FLAG_H|FLAG_N))|(c->AF.h&(FLAG_3|FLAG_5));
               cyc=4; break;
    case 0x37: c->AF.l=(c->AF.l&(FLAG_S|FLAG_Z|FLAG_PV))|FLAG_C|(c->AF.h&(FLAG_3|FLAG_5)); cyc=4; break;
    case 0x3F: { uint8_t f=c->AF.l;
                  c->AF.l=(f&(FLAG_S|FLAG_Z|FLAG_PV))|(f&FLAG_C?FLAG_H:FLAG_C)|(c->AF.h&(FLAG_3|FLAG_5));
                  cyc=4; break; }

    /* HALT */
    case 0x76: c->halted=1; cyc=4; break;

    /* LD r,r' (0x40-0x7F) */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: /* 0x76=HALT */ case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint8_t dst=(op>>3)&7, src=op&7;
        SET_R(dst, GET_R(src));
        cyc = (dst==6||src==6) ? 7 : 4;
        break;
    }

    /* ALU ops 0x80-0xBF */
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        c->AF.h=alu_add(c,c->AF.h,GET_R(op&7),0); cyc=(op&7)==6?7:4; break;
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        c->AF.h=alu_add(c,c->AF.h,GET_R(op&7),c->AF.l&FLAG_C?1:0); cyc=(op&7)==6?7:4; break;
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        c->AF.h=alu_sub(c,c->AF.h,GET_R(op&7),0); cyc=(op&7)==6?7:4; break;
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        c->AF.h=alu_sub(c,c->AF.h,GET_R(op&7),c->AF.l&FLAG_C?1:0); cyc=(op&7)==6?7:4; break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        alu_and(c,GET_R(op&7)); cyc=(op&7)==6?7:4; break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        alu_xor(c,GET_R(op&7)); cyc=(op&7)==6?7:4; break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        alu_or (c,GET_R(op&7)); cyc=(op&7)==6?7:4; break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        alu_cp (c,GET_R(op&7)); cyc=(op&7)==6?7:4; break;

    /* ALU A,n  0xC6 / 0xCE / 0xD6 / 0xDE / 0xE6 / 0xEE / 0xF6 / 0xFE */
    case 0xC6: c->AF.h=alu_add(c,c->AF.h,FETCH8(c),0); cyc=7; break;
    case 0xCE: c->AF.h=alu_add(c,c->AF.h,FETCH8(c),c->AF.l&FLAG_C?1:0); cyc=7; break;
    case 0xD6: c->AF.h=alu_sub(c,c->AF.h,FETCH8(c),0); cyc=7; break;
    case 0xDE: c->AF.h=alu_sub(c,c->AF.h,FETCH8(c),c->AF.l&FLAG_C?1:0); cyc=7; break;
    case 0xE6: alu_and(c,FETCH8(c)); cyc=7; break;
    case 0xEE: alu_xor(c,FETCH8(c)); cyc=7; break;
    case 0xF6: alu_or (c,FETCH8(c)); cyc=7; break;
    case 0xFE: alu_cp (c,FETCH8(c)); cyc=7; break;

    /* RET cc */
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
    case 0xE0: case 0xE8: case 0xF0: case 0xF8: {
        uint8_t cc=(op>>3)&7;
        if (COND(cc)) { c->PC=POP16(c); cyc=11; } else cyc=5;
        break;
    }
    /* POP rr */
    case 0xC1: c->BC.w=POP16(c); cyc=10; break;
    case 0xD1: c->DE.w=POP16(c); cyc=10; break;
    case 0xE1: c->HL.w=POP16(c); cyc=10; break;
    case 0xF1: c->AF.w=POP16(c); cyc=10; break;
    /* PUSH rr */
    case 0xC5: PUSH16(c,c->BC.w); cyc=11; break;
    case 0xD5: PUSH16(c,c->DE.w); cyc=11; break;
    case 0xE5: PUSH16(c,c->HL.w); cyc=11; break;
    case 0xF5: PUSH16(c,c->AF.w); cyc=11; break;

    /* JP cc */
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xE2: case 0xEA: case 0xF2: case 0xFA: {
        uint16_t a=FETCH16(c); uint8_t cc=(op>>3)&7;
        if (COND(cc)) c->PC=a; cyc=10; break;
    }
    case 0xC3: c->PC=FETCH16(c); cyc=10; break;
    case 0xE9: c->PC=c->HL.w;    cyc=4;  break;

    /* CALL cc */
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xE4: case 0xEC: case 0xF4: case 0xFC: {
        uint16_t a=FETCH16(c); uint8_t cc=(op>>3)&7;
        if (COND(cc)) { call_addr(c,a); cyc=17; } else cyc=10; break;
    }
    case 0xCD: call_addr(c, FETCH16(c)); cyc=17; break;
    case 0xC9: c->PC=POP16(c);           cyc=10; break;

    /* RST */
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        PUSH16(c,c->PC); c->PC=op&0x38; cyc=11; break;

    /* IN A,(n) / OUT (n),A */
    case 0xDB: { uint8_t n=FETCH8(c); c->AF.h=IN(c,((uint16_t)c->AF.h<<8)|n); cyc=11; break; }
    case 0xD3: { uint8_t n=FETCH8(c); OUT(c,((uint16_t)c->AF.h<<8)|n,c->AF.h); cyc=11; break; }

    /* EX / EXX */
    case 0xEB: { reg_pair_t t=c->DE; c->DE=c->HL; c->HL=t; cyc=4; break; }
    case 0xD9: {
        reg_pair_t t;
        t=c->BC; c->BC=c->BC_; c->BC_=t;
        t=c->DE; c->DE=c->DE_; c->DE_=t;
        t=c->HL; c->HL=c->HL_; c->HL_=t;
        cyc=4; break;
    }
    case 0xE3: { uint16_t t=READ16(c,c->SP); WRITE16(c,c->SP,c->HL.w); c->HL.w=t; cyc=19; break; }
    case 0xF9: c->SP=c->HL.w; cyc=6; break;

    /* DI / EI */
    case 0xF3: c->IFF1=c->IFF2=0; cyc=4; break;
    case 0xFB: c->IFF1=c->IFF2=1; cyc=4; break;

    /* LD SP,HL / LD HL,(SP) */
    //case 0xE5+0: /* already handled as PUSH HL */ break;

    /* Prefixes */
    case 0xCB: cyc=exec_cb(c); break;
    case 0xED: cyc=exec_ed(c); break;
    case 0xDD: cyc=exec_xy(c, &c->IX); break;
    case 0xFD: cyc=exec_xy(c, &c->IY); break;

    default: cyc=4; break; /* Unknown = NOP */
    }

    #undef GET_R
    #undef SET_R
    #undef GET_RP
    #undef SET_RP
    #undef COND

    return cyc;
}

int z80_run(z80_t *c, int cycles)
{
    int done = 0;
    while (done < cycles) {
        done += z80_step(c);
    }
    return done;
}

void z80_init(z80_t *c)
{
    memset(c, 0, sizeof(*c));
    z80_reset(c);
}

void z80_reset(z80_t *c)
{
    c->PC  = 0x0000;
    c->SP  = 0xFFFF;
    c->AF.w= 0xFFFF;
    c->BC.w= 0xFFFF;
    c->DE.w= 0xFFFF;
    c->HL.w= 0xFFFF;
    c->IX.w= 0xFFFF;
    c->IY.w= 0xFFFF;
    c->I   = 0;
    c->R   = 0;
    c->IFF1= 0;
    c->IFF2= 0;
    c->IM  = 0;
    c->halted = 0;
    c->nmi_pending = 0;
    c->int_pending = 0;
}

void z80_set_int(z80_t *c, int level)
{
    c->int_pending = level ? 1 : 0;
}

void z80_set_nmi(z80_t *c)
{
    c->nmi_pending = 1;
}
