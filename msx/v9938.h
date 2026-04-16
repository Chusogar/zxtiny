// v9938.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define V9938_VRAM_SIZE 0x20000   // 128 KB

typedef struct {
    // VRAM
    uint8_t  vram[V9938_VRAM_SIZE];

    // Registers & status
    uint8_t  reg[64];
    uint8_t  stat[10];

    // Control latch
    uint8_t  latch;
    bool     second;

    // VRAM address
    uint32_t addr;
    bool     write_mode;
    uint8_t  read_buf;

    // Palette
    uint16_t pal_rgb[16];     // RGB333 packed
    uint32_t pal_argb[16];
    bool     pal_second;
    uint8_t  pal_latch;

    // IRQ
    bool     irq;
} V9938;

void     v9938_reset(V9938* v);
uint8_t  v9938_read(V9938* v, uint8_t port);
void     v9938_write(V9938* v, uint8_t port, uint8_t val);

void     v9938_render_frame(V9938* v, uint32_t* fb, int w, int h);

static inline bool v9938_irq_pending(V9938* v) { return v->irq; }
static inline void v9938_irq_clear(V9938* v)   { v->irq=false; }
