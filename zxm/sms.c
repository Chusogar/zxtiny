/*
 * sms.c  -  Emulador de Sega Master System
 *
 * Estructura y estilo basados en zx.c y el resto del proyecto zxtiny.
 * Referencia principal: Charles MacDonald "Sega Master System VDP documentation"
 * y la guia de codeslinger.co.uk.
 */

#include "sms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Paleta SMS: formato 2 bits por canal (BGR)
// CRAM byte: bits 1-0 = R, bits 3-2 = G, bits 5-4 = B
// Cada canal: 0→0, 1→85, 2→170, 3→255
// ---------------------------------------------------------------------------
static const uint8_t sms_chan[4] = { 0, 85, 170, 255 };

static void vdp_update_palette(VDP* v, int idx) {
    uint8_t c = v->cram[idx];
    uint8_t r = sms_chan[(c >> 0) & 3];
    uint8_t g = sms_chan[(c >> 2) & 3];
    uint8_t b = sms_chan[(c >> 4) & 3];
    v->palette[idx] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
}

// ---------------------------------------------------------------------------
// Mapper Sega: acceso a ROM con paginacion
// ---------------------------------------------------------------------------

static inline uint8_t* rom_page_ptr(SMS* s, int slot) {
    int bank = s->slot[slot] % s->num_banks;
    return s->rom + bank * SMS_ROM_SLOT_SIZE;
}

static uint8_t rom_read(SMS* s, uint16_t addr) {
    if (addr < SMS_FIRST_KB)
        return s->rom[addr];  // primer 1KB siempre fijo (banco 0)
    if (addr < 0x4000)
        return rom_page_ptr(s, 0)[addr];           // slot 0
    if (addr < 0x8000)
        return rom_page_ptr(s, 1)[addr - 0x4000];  // slot 1
    return rom_page_ptr(s, 2)[addr - 0x8000];      // slot 2
}

static void mapper_write(SMS* s, uint16_t addr, uint8_t val) {
    // 0xFFFC: page control (RAM mapping, ignorado para simplificar)
    // 0xFFFC-0xFFFF: bank select
    switch (addr) {
    case 0xFFFC: break; // page control (RAM-in-slot, ignorado)
    case 0xFFFD: s->slot[0] = val & 0x3F; break;
    case 0xFFFE: s->slot[1] = val & 0x3F; break;
    case 0xFFFF: s->slot[2] = val & 0x3F; break;
    }
}

// ---------------------------------------------------------------------------
// VDP: escritura al puerto de control (0xBF)
// ---------------------------------------------------------------------------

static void vdp_write_ctrl(VDP* v, uint8_t data) {
    if (!v->ctrl_second) {
        // primer byte: byte bajo del control word
        v->ctrl_word = (v->ctrl_word & 0xFF00) | data;
        v->ctrl_second = true;
    } else {
        // segundo byte: byte alto
        v->ctrl_word = (v->ctrl_word & 0x00FF) | ((uint16_t)data << 8);
        v->ctrl_second = false;

        uint8_t code = (uint8_t)(v->ctrl_word >> 14);
        switch (code) {
        case 0:  // leer VRAM: precargar read buffer
            v->read_buf = v->vram[v->ctrl_word & 0x3FFF];
            v->ctrl_word = (v->ctrl_word & 0xC000) | ((v->ctrl_word + 1) & 0x3FFF);
            break;
        case 2: {  // escribir registro VDP
            uint8_t reg = (v->ctrl_word >> 8) & 0x0F;
            uint8_t val2 = (uint8_t)(v->ctrl_word & 0xFF);
            if (reg <= 10) {
                v->reg[reg] = val2;
                // si se habilitan interrupciones VSync con IRQ pendiente
                if (reg == 1 && (v->status & 0x80) && (val2 & 0x20))
                    v->irq_pending = true;
            }
            break;
        }
        default: break;  // 1 y 3: solo actualizar addr
        }
    }
}

// VDP: escritura al puerto de datos (0xBE)
static void vdp_write_data(VDP* v, uint8_t data) {
    v->ctrl_second = false;
    uint8_t code = (uint8_t)(v->ctrl_word >> 14);
    uint16_t addr = v->ctrl_word & 0x3FFF;

    if (code == 3) {
        // escribe en CRAM
        int cidx = addr & 0x1F;
        v->cram[cidx] = data;
        vdp_update_palette(v, cidx);
    } else {
        // escribe en VRAM
        v->vram[addr] = data;
    }
    v->read_buf = data;
    // incrementar dirección
    uint16_t new_addr = (addr + 1) & 0x3FFF;
    v->ctrl_word = (v->ctrl_word & 0xC000) | new_addr;
}

// VDP: lectura del puerto de datos (0xBE)
static uint8_t vdp_read_data(VDP* v) {
    v->ctrl_second = false;
    uint8_t res = v->read_buf;
    uint16_t addr = v->ctrl_word & 0x3FFF;
    v->read_buf = v->vram[addr];
    uint16_t new_addr = (addr + 1) & 0x3FFF;
    v->ctrl_word = (v->ctrl_word & 0xC000) | new_addr;
    return res;
}

// VDP: lectura del status register (0xBF)
static uint8_t vdp_read_status(VDP* v) {
    uint8_t res = v->status;
    v->status &= 0x1F;   // clear bits 7-5
    v->ctrl_second = false;
    v->irq_pending = false;
    return res;
}

// ---------------------------------------------------------------------------
// VDP: calcular address de la name table
// ---------------------------------------------------------------------------
static uint16_t vdp_name_table_addr(VDP* v) {
    bool med_or_large = (v->reg[1] & 0x18) && (v->reg[0] & 0x02);
    if (med_or_large)
        return (uint16_t)(((v->reg[2] & 0x0C) << 10) | 0x0700);
    else
        return (uint16_t)((v->reg[2] & 0x0E) << 10);
}

// VDP: calcular address de la sprite attribute table
static uint16_t vdp_sprite_table_addr(VDP* v) {
    return (uint16_t)((v->reg[5] & 0x7E) << 7);
}

// ---------------------------------------------------------------------------
// VDP: renderizar un scanline (modo 4)
// ---------------------------------------------------------------------------

static void vdp_render_line(VDP* v, int line) {
    if (line < 0 || line >= v->height) return;

    bool screen_enabled = (v->reg[1] & 0x40) != 0;
    uint32_t* row = &v->framebuffer[line * SMS_SCREEN_W];

    // color de fondo (overscan): CRAM[16 + (reg7 & 0xF)]
    int bg_color_idx = 16 + (v->reg[7] & 0x0F);
    uint32_t bg_col = v->palette[bg_color_idx];

    if (!screen_enabled) {
        for (int x = 0; x < SMS_SCREEN_W; x++) row[x] = bg_col;
        return;
    }

    // ── Background tiles ─────────────────────────────────────────────────
    uint16_t nt_addr  = vdp_name_table_addr(v);
    uint8_t  scrollx  = v->reg[8];
    uint8_t  scrolly  = v->vscroll;

    // columna de tile subyacente y pixel dentro del tile
    // la pantalla tiene 32 tiles de ancho, desplazados por scrollx
    // scrollx se suma desde la izquierda (scroll hacia la derecha)

    uint8_t prio_line[SMS_SCREEN_W] = {0}; // prioridad por pixel

    for (int tx = 0; tx < 32; tx++) {
        // scroll X: disabled for columns 24-31 si reg0.bit7
        int sx_off = (v->reg[0] & 0x40) ? 0 : scrollx;
        // desplazamiento en pixeles desde la izquierda
        int px_base = (tx * 8 - sx_off) & 0xFF;
        // si px_base >= 256 wrapeamos
        // fila de tile con scroll Y
        int ty = ((line + scrolly) >> 3) & 0x1F;  // tile row (0-31)
        int fy = ((line + scrolly)) & 7;           // pixel row dentro del tile

        // address en la name table (32x28 tiles en modo 4 small = 896 entries)
        // En small (192px): 32 cols x 28 rows, solo se usan filas 0-27
        // Nota: la name table en modo 4 small tiene 32x32 tiles pero solo
        // las primeras 28 filas son visibles (192px / 8 = 24 filas activas,
        // mas el scroll). Para simplificar usamos 32x32.
        uint16_t nt_off = (uint16_t)(nt_addr + ty * 32 * 2 + tx * 2);
        nt_off &= 0x3FFF;
        if (nt_off + 1 >= SMS_VRAM_SIZE) continue;

        uint8_t b0 = v->vram[nt_off];
        uint8_t b1 = v->vram[nt_off + 1];
        int tile    = b0 | ((b1 & 0x01) << 8);  // 9 bits de tile number
        bool flipx  = (b1 & 0x02) != 0;
        bool flipy  = (b1 & 0x04) != 0;
        bool pal1   = (b1 & 0x08) != 0;
        bool prio   = (b1 & 0x10) != 0;

        // fila del tile con flip Y
        int tile_row = flipy ? (7 - fy) : fy;

        // 4 bytes por fila de tile en VRAM (32 bytes por tile)
        uint32_t tile_addr = (uint32_t)(tile * 32 + tile_row * 4);
        if (tile_addr + 3 >= SMS_VRAM_SIZE) continue;

        // 4 bytes = 32 bits = 8 pixels (4bpp planar)
        uint8_t p0 = v->vram[tile_addr];
        uint8_t p1 = v->vram[tile_addr + 1];
        uint8_t p2 = v->vram[tile_addr + 2];
        uint8_t p3 = v->vram[tile_addr + 3];

        for (int fx = 0; fx < 8; fx++) {
            int bit = flipx ? fx : (7 - fx);
            uint8_t pix = (uint8_t)(((p0 >> bit) & 1)
                         | (((p1 >> bit) & 1) << 1)
                         | (((p2 >> bit) & 1) << 2)
                         | (((p3 >> bit) & 1) << 3));
            int screen_x = (px_base + fx) & 0xFF;
            if (screen_x >= SMS_SCREEN_W) continue;

            // paleta 0 = CRAM[0-15], paleta 1 = CRAM[16-31]
            int cidx = pal1 ? (16 + pix) : pix;
            row[screen_x] = v->palette[cidx];
            if (prio) prio_line[screen_x] = 1;  // tile prioritario sobre sprites
        }
    }

    // columna 0 forzada al color de overscan si reg0.bit5
    if (v->reg[0] & 0x20) {
        for (int x = 0; x < 8; x++) row[x] = bg_col;
    }

    // ── Sprites ──────────────────────────────────────────────────────────
    uint16_t sat_addr = vdp_sprite_table_addr(v);
    bool spr_16 = (v->reg[1] & 0x02) != 0;  // 8x16 si bit1=1
    bool spr_zoom = (v->reg[1] & 0x01) != 0;
    // sprites en patrón 0x2000 si reg6.bit2
    uint32_t spr_pat_base = (v->reg[6] & 0x04) ? 0x2000u : 0u;
    // los sprites usan bit3=0 de reg6 para el bit extra del tile? No,
    // en modo 4 los sprites siempre empiezan en spr_pat_base.

    // La sprite attribute table (SAT) tiene:
    //   bytes 0x00-0x3F: Y positions de 64 sprites (1 byte cada uno)
    //   bytes 0x80-0xFF: X (1 byte) + tile number (1 byte) por sprite
    // El byte Y=0xD0 es el sentinel que detiene el procesado de sprites.

    int spr_height = spr_16 ? 16 : 8;
    if (spr_zoom) spr_height *= 2;
    int spr_count = 0;
    int spr_overflow = 0;

    for (int sn = 0; sn < 64; sn++) {
        uint16_t y_addr = (uint16_t)((sat_addr + sn) & 0x3FFF);
        int sy = (int)v->vram[y_addr];
        if (sy == 0xD0) break;  // sentinel
        sy++;  // Y+1 (los sprites se dibujan una linea mas abajo)
        if (spr_zoom) sy++;

        // ¿intersecta este sprite con la linea actual?
        if (line < sy || line >= sy + spr_height) continue;

        spr_count++;
        if (spr_count > 8) { spr_overflow = 1; break; }

        // leer X y tile de la parte alta de la SAT
        uint16_t x_addr = (uint16_t)((sat_addr + 0x80 + sn * 2) & 0x3FFF);
        int sx   = (int)v->vram[x_addr];
        int tile2 = (int)v->vram[(uint16_t)((x_addr + 1) & 0x3FFF)];

        // bit3 de reg0: desplazar sprites 8 pixels a la izquierda
        if (v->reg[0] & 0x08) sx -= 8;

        // fila del sprite en su propio espacio
        int spr_row = line - sy;
        if (spr_zoom) spr_row >>= 1;

        // en sprites 8x16: bit 0 del tile number se ignora
        if (spr_16) { tile2 &= ~1; if (spr_row >= 8) { tile2 |= 1; spr_row -= 8; } }

        uint32_t pat_addr = spr_pat_base + (uint32_t)(tile2 * 32 + spr_row * 4);
        if (pat_addr + 3 >= SMS_VRAM_SIZE) continue;

        uint8_t p0 = v->vram[pat_addr];
        uint8_t p1 = v->vram[pat_addr + 1];
        uint8_t p2 = v->vram[pat_addr + 2];
        uint8_t p3 = v->vram[pat_addr + 3];

        for (int fx = 0; fx < 8; fx++) {
            int bit = 7 - fx;
            uint8_t pix = (uint8_t)(((p0>>bit)&1) | (((p1>>bit)&1)<<1)
                         | (((p2>>bit)&1)<<2) | (((p3>>bit)&1)<<3));
            if (pix == 0) continue;  // transparente

            int draw_w = spr_zoom ? 2 : 1;
            for (int dw = 0; dw < draw_w; dw++) {
                int screen_x = sx + fx * draw_w + dw;
                if (screen_x < 0 || screen_x >= SMS_SCREEN_W) continue;
                if (prio_line[screen_x]) continue;  // bg prioritario
                // sprites usan paleta 1 (CRAM 16-31)
                row[screen_x] = v->palette[16 + pix];
            }
        }
    }

    if (spr_overflow) v->status |= 0x40;
}

// ---------------------------------------------------------------------------
// VDP: avanzar un scanline (llamado desde sms_run_frame)
// Devuelve true si hay IRQ pendiente para la CPU.
// ---------------------------------------------------------------------------

static bool vdp_tick_line(VDP* v) {
    bool irq = false;
    uint8_t vcount = v->vcounter;

    // ¿Estamos en zona activa?
    bool active = (vcount < (uint8_t)v->height);

    if (active) {
        // Renderizar esta línea
        vdp_render_line(v, vcount);

        // Decrementar contador de línea
        if (v->line_counter == 0) {
            v->line_counter = (int8_t)v->reg[0x0A];
            if (v->reg[0] & 0x10) {  // line IRQ habilitada
                v->line_irq = true;
                irq = true;
            }
        } else {
            v->line_counter--;
        }
    } else {
        // Zona inactiva
        if (vcount == (uint8_t)v->height) {
            // Primera línea del blanking: VSYNC IRQ
            v->status |= 0x80;
            if (v->reg[1] & 0x20) {
                v->irq_pending = true;
                irq = true;
            }
            // Recargar contador de línea
            v->line_counter = (int8_t)v->reg[0x0A];
        }

        // Actualizar scroll Y solo en zona inactiva
        v->vscroll = v->reg[9];

        // Actualizar altura activa según modo
        bool mode4 = (v->reg[0] & 0x04) != 0;
        bool m2    = (v->reg[0] & 0x02) != 0;
        if (mode4 && m2) {
            if (v->reg[1] & 0x08)       v->height = SMS_SCREEN_H_LARGE;
            else if (v->reg[1] & 0x04)  v->height = SMS_SCREEN_H_MED;
            else                         v->height = SMS_SCREEN_H_SMALL;
        } else {
            v->height = SMS_SCREEN_H_SMALL;
        }
    }

    // Avanzar vcounter
    v->vcounter++;
    if (vcount == 0xFF) {
        v->vcounter = 0;
        v->vcnt_jumped = false;
    } else if (vcount == SMS_VCNT_JUMP_FROM && !v->vcnt_jumped) {
        v->vcounter  = SMS_VCNT_JUMP_TO;
        v->vcnt_jumped = true;
    }

    // IRQ combinada
    if (v->irq_pending) irq = true;
    return irq;
}

// ---------------------------------------------------------------------------
// Callbacks Z80
// ---------------------------------------------------------------------------

static uint8_t mem_read(void* ud, uint16_t addr) {
    SMS* s = (SMS*)ud;
    if (addr <= 0xBFFF) return rom_read(s, addr);
    // RAM: 0xC000-0xFFFF (8KB espejada)
    return s->ram[addr & (SMS_RAM_SIZE - 1)];
}

static void mem_write(void* ud, uint16_t addr, uint8_t val) {
    SMS* s = (SMS*)ud;
    if (addr <= 0xBFFF) return;  // ROM: solo lectura
    // RAM + mapper
    s->ram[addr & (SMS_RAM_SIZE - 1)] = val;
    // Mapper registers en 0xFFFC-0xFFFF
    if (addr >= 0xFFFC) mapper_write(s, addr, val);
}

static uint8_t port_in(z80* z, uint16_t port) {
    SMS* s = (SMS*)z->userdata;
    uint8_t p = (uint8_t)(port & 0xFF);

    switch (p) {
    case 0x7E: return s->vdp.vcounter;
    case 0x7F: return s->vdp.hcounter;
    case 0xBE: return vdp_read_data(&s->vdp);
    case 0xBF:
    case 0xBD: return vdp_read_status(&s->vdp);
    case 0xDC:
    case 0xC0: return s->port_dc;
    case 0xDD:
    case 0xC1: return s->port_dd;
    default:   return 0xFF;
    }
}

static void port_out(z80* z, uint16_t port, uint8_t val) {
    SMS* s = (SMS*)z->userdata;
    uint8_t p = (uint8_t)(port & 0xFF);

    switch (p) {
    case 0x7E:
    case 0x7F:  /* SN76489 PSG: ignorado */ break;
    case 0xBE:  vdp_write_data(&s->vdp, val); break;
    case 0xBF:
    case 0xBD:  vdp_write_ctrl(&s->vdp, val); break;
    default:    break;
    }
}

// ---------------------------------------------------------------------------
// sms_init
// ---------------------------------------------------------------------------

void sms_init(SMS* s) {
    memset(s, 0, sizeof(*s));

    // VDP defaults
    s->vdp.height = SMS_SCREEN_H_SMALL;
    s->vdp.line_counter = 0xFF;
    s->vdp.vscroll = 0;
    s->vdp.vcnt_jumped = false;
    // Paleta inicial (negro)
    for (int i = 0; i < SMS_CRAM_SIZE; i++) vdp_update_palette(&s->vdp, i);

    // Input: todo a 1 (active-low = sin pulsar)
    s->port_dc = 0xFF;
    s->port_dd = 0xFF;

    // Mapper: bancos iniciales 0, 1, 2
    s->slot[0] = 0;
    s->slot[1] = 1;
    s->slot[2] = 2;

    // CPU Z80
    z80_init(&s->cpu);
    s->cpu.userdata   = s;
    s->cpu.read_byte  = mem_read;
    s->cpu.write_byte = mem_write;
    s->cpu.port_in    = port_in;
    s->cpu.port_out   = port_out;

    // SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    s->window = SDL_CreateWindow("Sega Master System",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SMS_SCREEN_W * SMS_SCALE, SMS_SCREEN_H_SMALL * SMS_SCALE,
        SDL_WINDOW_SHOWN);
    s->renderer = SDL_CreateRenderer(s->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    s->texture = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, SMS_SCREEN_W, SMS_SCREEN_H_SMALL);
    s->tex_h = SMS_SCREEN_H_SMALL;
    SDL_RenderSetLogicalSize(s->renderer,
        SMS_SCREEN_W * SMS_SCALE, SMS_SCREEN_H_SMALL * SMS_SCALE);
}

void sms_destroy(SMS* s) {
    if (s->rom) { free(s->rom); s->rom = NULL; }
    if (s->texture)  SDL_DestroyTexture(s->texture);
    if (s->renderer) SDL_DestroyRenderer(s->renderer);
    if (s->window)   SDL_DestroyWindow(s->window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// sms_load_rom
// ---------------------------------------------------------------------------

int sms_load_rom(SMS* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "No se puede abrir '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    // Las ROMs SMS pueden tener un header de 512 bytes al principio
    long rom_offset = 0;
    if ((size & 0x3FF) == 0x200) { rom_offset = 0x200; size -= 0x200; }

    if (size > SMS_ROM_MAXSIZE) {
        fprintf(stderr, "ROM demasiado grande: %ld bytes\n", size);
        fclose(f); return -1;
    }

    s->rom = calloc(1, (size_t)SMS_ROM_MAXSIZE);
    if (!s->rom) { fclose(f); return -1; }

    fseek(f, rom_offset, SEEK_SET);
    int n = (int)fread(s->rom, 1, (size_t)size, f);
    fclose(f);

    s->rom_size  = n;
    s->num_banks = (n + SMS_ROM_SLOT_SIZE - 1) / SMS_ROM_SLOT_SIZE;
    if (s->num_banks < 1) s->num_banks = 1;

    // Para ROMs pequeñas (<= 48KB): no hace falta mapper
    // Para ROMs grandes: el mapper se activa por escrituras en 0xFFFC-0xFFFF

    printf("[ROM] '%s' cargada: %d bytes, %d bancos de 16KB\n",
           path, n, s->num_banks);

    // Extraer nombre
    const char* base = path;
    for (const char* p2 = path; *p2; p2++) if (*p2=='/'||*p2=='\\') base=p2+1;
    strncpy(s->rom_name, base, sizeof(s->rom_name)-1);

    // Actualizar titulo de ventana
    char title[300];
    snprintf(title, sizeof(title), "SMS: %s", s->rom_name);
    SDL_SetWindowTitle(s->window, title);

    return 0;
}

// ---------------------------------------------------------------------------
// sms_run_frame
// Bucle: ejecutar 228 ciclos Z80 por scanline, luego avanzar el VDP.
// ---------------------------------------------------------------------------

void sms_run_frame(SMS* s) {
    for (int line = 0; line < SMS_SCANLINES; line++) {
        // Ejecutar ~228 ciclos de Z80 para este scanline
        z80_step_n(&s->cpu, SMS_Z80_PER_LINE);

        // Avanzar VDP un scanline; si genera IRQ, señalarla a la CPU
        bool irq = vdp_tick_line(&s->vdp);
        if (irq) z80_pulse_irq(&s->cpu, 0xFF);
    }
    s->frame_counter++;
}

// ---------------------------------------------------------------------------
// sms_render
// ---------------------------------------------------------------------------

void sms_render(SMS* s) {
    int h = s->vdp.height;

    // Recrear textura si cambió la altura
    if (h != s->tex_h) {
        SDL_DestroyTexture(s->texture);
        s->texture = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, SMS_SCREEN_W, h);
        SDL_RenderSetLogicalSize(s->renderer, SMS_SCREEN_W * SMS_SCALE, h * SMS_SCALE);
        s->tex_h = h;
    }

    SDL_UpdateTexture(s->texture, NULL, s->vdp.framebuffer,
                      SMS_SCREEN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(s->renderer);
    SDL_Rect dst = { 0, 0, SMS_SCREEN_W * SMS_SCALE, h * SMS_SCALE };
    SDL_RenderCopy(s->renderer, s->texture, NULL, &dst);
    SDL_RenderPresent(s->renderer);
}

// ---------------------------------------------------------------------------
// sms_handle_key
// ---------------------------------------------------------------------------

void sms_handle_key(SMS* s, SDL_Scancode sc, bool pressed) {
    // port_dc: P1 joystick (bits 0-5), P2 up/down (bits 6-7) — active-low
    // port_dd: P2 left/right/fire (bits 0-3), reset (bit 4) — active-low

    uint8_t* reg = NULL;
    uint8_t  bit = 0;

    switch (sc) {
    // P1
    case SDL_SCANCODE_UP:    reg=&s->port_dc; bit=SMS_P1_UP;    break;
    case SDL_SCANCODE_DOWN:  reg=&s->port_dc; bit=SMS_P1_DOWN;  break;
    case SDL_SCANCODE_LEFT:  reg=&s->port_dc; bit=SMS_P1_LEFT;  break;
    case SDL_SCANCODE_RIGHT: reg=&s->port_dc; bit=SMS_P1_RIGHT; break;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_Z:     reg=&s->port_dc; bit=SMS_P1_FIRE_A; break;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_X:     reg=&s->port_dc; bit=SMS_P1_FIRE_B; break;
    // P2 (WASD)
    case SDL_SCANCODE_W:     reg=&s->port_dc; bit=SMS_P2_UP;    break;
    case SDL_SCANCODE_S:     reg=&s->port_dc; bit=SMS_P2_DOWN;  break;
    case SDL_SCANCODE_A:     reg=&s->port_dd; bit=SMS_P2_LEFT;  break;
    case SDL_SCANCODE_D:     reg=&s->port_dd; bit=SMS_P2_RIGHT; break;
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_C:     reg=&s->port_dd; bit=SMS_P2_FIRE_A; break;
    case SDL_SCANCODE_RALT:
    case SDL_SCANCODE_V:     reg=&s->port_dd; bit=SMS_P2_FIRE_B; break;
    // Reset
    case SDL_SCANCODE_R:     reg=&s->port_dd; bit=SMS_RESET_BTN; break;
    // Pause = NMI (simplificado: no implementado)
    case SDL_SCANCODE_RETURN:
        if (pressed) z80_pulse_nmi(&s->cpu);
        return;
    case SDL_SCANCODE_ESCAPE:
        if (pressed) { s->quit = true; } return;
    case SDL_SCANCODE_F5:
        // Reiniciar
        if (pressed) {
            z80_init(&s->cpu);
            s->cpu.userdata   = s;
            s->cpu.read_byte  = mem_read;
            s->cpu.write_byte = mem_write;
            s->cpu.port_in    = port_in;
            s->cpu.port_out   = port_out;
            memset(&s->vdp, 0, sizeof(s->vdp));
            s->vdp.height = SMS_SCREEN_H_SMALL;
            s->vdp.line_counter = 0xFF;
            for (int i=0;i<SMS_CRAM_SIZE;i++) vdp_update_palette(&s->vdp,i);
            s->slot[0]=0; s->slot[1]=1; s->slot[2]=2;
            printf("[SMS] Reset\n");
        }
        return;
    default: return;
    }

    if (reg) {
        if (pressed) *reg &= ~bit;   // active-low: presionar = poner a 0
        else         *reg |=  bit;   // soltar = poner a 1
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void print_usage(const char* exe) {
    printf("Uso: %s <rom.sms>\n\n", exe);
    printf("Emulador de Sega Master System\n\n");
    printf("Controles:\n");
    printf("  Flechas      Joystick P1\n");
    printf("  Ctrl/Z       Boton 1 P1\n");
    printf("  Alt/X        Boton 2 P1\n");
    printf("  W/A/S/D      Joystick P2\n");
    printf("  RCtrl/C      Boton 1 P2\n");
    printf("  RAlt/V       Boton 2 P2\n");
    printf("  Enter        Pause (NMI)\n");
    printf("  R            Reset button\n");
    printf("  F5           Reiniciar emulador\n");
    printf("  Escape       Salir\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    static SMS s;
    sms_init(&s);

    if (sms_load_rom(&s, argv[1]) != 0) {
        fprintf(stderr, "Error cargando ROM '%s'\n", argv[1]);
        sms_destroy(&s);
        return 1;
    }

    const uint32_t FRAME_MS = 1000 / SMS_FPS;
    printf("SMS - iniciando emulacion. ESC para salir.\n");

    while (!s.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) s.quit = true;
            else if (e.type == SDL_KEYDOWN) sms_handle_key(&s, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)   sms_handle_key(&s, e.key.keysym.scancode, false);
            else if (e.type == SDL_DROPFILE) {
                // Arrastrar ROM encima de la ventana
                sms_load_rom(&s, e.drop.file);
                SDL_free(e.drop.file);
                // Reset Z80
                z80_init(&s.cpu);
                s.cpu.userdata=&s; s.cpu.read_byte=mem_read;
                s.cpu.write_byte=mem_write; s.cpu.port_in=port_in;
                s.cpu.port_out=port_out;
            }
        }

        sms_run_frame(&s);
        sms_render(&s);

        uint32_t elapsed = SDL_GetTicks() - t0;
        if (elapsed < FRAME_MS) SDL_Delay(FRAME_MS - elapsed);
    }

    sms_destroy(&s);
    return 0;
}
