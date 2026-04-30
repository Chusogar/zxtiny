#include "c64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static C64 c64;

// =============================================================================
// SID 6581 - emulación simplificada
// =============================================================================

static const uint16_t sid_attack_rates[16] = {
    2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000
};

static const uint16_t sid_decrel_rates[16] = {
    6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000
};

static void sid_reset(C64* c) {
    memset(c->sid_regs, 0, sizeof(c->sid_regs));
    for (int i = 0; i < SID_NUM_VOICES; i++) {
        memset(&c->sid_voice[i], 0, sizeof(SIDVoice));
        c->sid_voice[i].shift_reg = 0x7FFFF8;
    }
    c->sid_volume = 0;
}

static void sid_write(C64* c, uint8_t reg, uint8_t val) {
    if (reg >= SID_NUM_REGS) return;
    c->sid_regs[reg] = val;

    if (reg == 0x18) {
        c->sid_volume = val & 0x0F;
        c->sid_filter_mode = (val >> 4) & 0x0F;
    } else if (reg == 0x17) {
        c->sid_filter_resonance = (val >> 4) & 0x0F;
        c->sid_filter_route = val & 0x0F;
    } else if (reg == 0x15 || reg == 0x16) {
        c->sid_filter_cutoff = (c->sid_regs[0x15] & 0x07) |
                               ((uint16_t)c->sid_regs[0x16] << 3);
    }
}

static uint8_t sid_read(C64* c, uint8_t reg) {
    if (reg == 0x19) return 0xFF; // potx
    if (reg == 0x1A) return 0xFF; // poty
    if (reg == 0x1B) { // osc3 readback
        SIDVoice* v = &c->sid_voice[2];
        return (uint8_t)(v->accumulator >> 16);
    }
    if (reg == 0x1C) { // env3 readback
        return (uint8_t)c->sid_voice[2].env_counter;
    }
    return 0;
}

static float sid_generate_sample(C64* c) {
    float output = 0.0f;

    for (int ch = 0; ch < SID_NUM_VOICES; ch++) {
        int base = ch * 7;
        SIDVoice* v = &c->sid_voice[ch];

        uint16_t freq = (uint16_t)c->sid_regs[base] |
                        ((uint16_t)c->sid_regs[base + 1] << 8);
        uint16_t pw   = ((uint16_t)c->sid_regs[base + 2] |
                        ((uint16_t)(c->sid_regs[base + 3] & 0x0F) << 8));
        uint8_t ctrl  = c->sid_regs[base + 4];
        uint8_t ad    = c->sid_regs[base + 5];
        uint8_t sr    = c->sid_regs[base + 6];

        bool gate = (ctrl & 0x01) != 0;

        // Gate edge detection
        if (gate && !v->gate_prev) {
            v->env_state = 1; // attack
            v->env_rate_counter = 0;
        } else if (!gate && v->gate_prev) {
            v->env_state = 4; // release
        }
        v->gate_prev = gate;

        // Envelope
        uint16_t rate;
        switch (v->env_state) {
        case 1: // attack
            rate = sid_attack_rates[(ad >> 4) & 0x0F];
            v->env_rate_counter++;
            if (v->env_rate_counter >= rate) {
                v->env_rate_counter = 0;
                if (v->env_counter < 255) {
                    v->env_counter++;
                } else {
                    v->env_state = 2; // decay
                }
            }
            break;
        case 2: // decay
            rate = sid_decrel_rates[ad & 0x0F];
            v->env_rate_counter++;
            if (v->env_rate_counter >= rate) {
                v->env_rate_counter = 0;
                uint8_t sustain = ((sr >> 4) & 0x0F) * 17;
                if (v->env_counter > sustain) {
                    v->env_counter--;
                } else {
                    v->env_state = 3; // sustain
                }
            }
            break;
        case 3: // sustain
            v->env_counter = ((sr >> 4) & 0x0F) * 17;
            break;
        case 4: // release
            rate = sid_decrel_rates[sr & 0x0F];
            v->env_rate_counter++;
            if (v->env_rate_counter >= rate) {
                v->env_rate_counter = 0;
                if (v->env_counter > 0)
                    v->env_counter--;
            }
            break;
        default:
            break;
        }

        // Oscillator
        v->accumulator += freq;
        v->accumulator &= 0xFFFFFF;

        uint16_t osc = 0;
        uint8_t waveform = (ctrl >> 4) & 0x0F;

        if (waveform & 0x08) { // noise
            // Shift on bit 19 transition
            if ((v->accumulator & 0x080000) && !((v->accumulator - freq) & 0x080000)) {
                uint32_t bit = ((v->shift_reg >> 22) ^ (v->shift_reg >> 17)) & 1;
                v->shift_reg = ((v->shift_reg << 1) | bit) & 0x7FFFFF;
            }
            osc = (uint16_t)(
                ((v->shift_reg >> 15) & 0x800) |
                ((v->shift_reg >> 14) & 0x400) |
                ((v->shift_reg >> 11) & 0x200) |
                ((v->shift_reg >>  9) & 0x100) |
                ((v->shift_reg >>  6) & 0x080) |
                ((v->shift_reg >>  3) & 0x040) |
                ((v->shift_reg >>  1) & 0x020) |
                ((v->shift_reg      ) & 0x010)
            );
        } else {
            if (waveform & 0x01) { // triangle
                uint32_t msb = v->accumulator >> 23;
                osc = (uint16_t)((v->accumulator >> 11) & 0xFFF);
                if (msb) osc ^= 0xFFF;
            }
            if (waveform & 0x02) { // sawtooth
                osc = (uint16_t)((v->accumulator >> 12) & 0xFFF);
            }
            if (waveform & 0x04) { // pulse
                osc = (v->accumulator >> 12) >= pw ? 0xFFF : 0;
            }
        }

        // Test bit resets oscillator
        if (ctrl & 0x08) {
            v->accumulator = 0;
            osc = 0;
        }

        float sample = ((float)osc / 4095.0f) * 2.0f - 1.0f;
        float envelope = (float)v->env_counter / 255.0f;
        output += sample * envelope;
    }

    float vol = (float)c->sid_volume / 15.0f;
    return (output / 3.0f) * vol;
}

// =============================================================================
// CIA 6526 - timers e interrupciones
// =============================================================================

static void cia_reset(CIA* ci) {
    memset(ci, 0, sizeof(CIA));
    ci->pra = 0xFF;
    ci->prb = 0xFF;
    ci->latch_a = 0xFFFF;
    ci->latch_b = 0xFFFF;
    ci->timer_a = 0xFFFF;
    ci->timer_b = 0xFFFF;
    ci->tod_running = true;
}

static void cia_step(CIA* ci, int cycles) {
    // Timer A
    if (ci->cra & 0x01) { // timer running
        if ((ci->cra & 0x20) == 0) { // count phi2 clocks
            if (ci->timer_a <= (uint16_t)cycles) {
                ci->timer_a = ci->latch_a; // reload
                ci->icr |= 0x01; // timer A underflow
                if (ci->cra & 0x08) // one-shot
                    ci->cra &= ~0x01;
            } else {
                ci->timer_a -= (uint16_t)cycles;
            }
        }
    }

    // Timer B
    if (ci->crb & 0x01) {
        uint8_t mode = (ci->crb >> 5) & 0x03;
        if (mode == 0) { // count phi2
            if (ci->timer_b <= (uint16_t)cycles) {
                ci->timer_b = ci->latch_b;
                ci->icr |= 0x02;
                if (ci->crb & 0x08)
                    ci->crb &= ~0x01;
            } else {
                ci->timer_b -= (uint16_t)cycles;
            }
        }
        // mode 2 = count timer A underflows (simplified: handled above)
    }
}

static uint8_t cia_read(C64* c, int idx, uint8_t reg) {
    CIA* ci = &c->cia[idx];
    switch (reg & 0x0F) {
    case 0x00: // PRA
        if (idx == 0) {
            // CIA1 PRA: keyboard column + joystick 2
            uint8_t val = 0xFF;
            uint8_t rows = ~ci->prb; // selected rows
            for (int i = 0; i < 8; i++) {
                if (rows & (1 << i))
                    val &= c->keyboard_matrix[i];
            }
            val &= c->joystick; // joystick 2
            return val;
        }
        return (ci->pra | ~ci->ddra);
    case 0x01: // PRB
        if (idx == 0) {
            // CIA1 PRB: keyboard row read only - NO tape signal here
            uint8_t val = 0xFF;
            uint8_t cols = ~ci->pra;
            for (int i = 0; i < 8; i++) {
                if (cols & (1 << i)) {
                    for (int j = 0; j < 8; j++) {
                        if (!(c->keyboard_matrix[j] & (1 << i)))
                            val &= ~(1 << j);
                    }
                }
            }
            return val;
        }
        return (ci->prb | ~ci->ddrb);
    case 0x02: return ci->ddra;
    case 0x03: return ci->ddrb;
    case 0x04: return (uint8_t)(ci->timer_a & 0xFF);
    case 0x05: return (uint8_t)(ci->timer_a >> 8);
    case 0x06: return (uint8_t)(ci->timer_b & 0xFF);
    case 0x07: return (uint8_t)(ci->timer_b >> 8);
    case 0x08: return ci->tod[0]; // TOD 10ths
    case 0x09: return ci->tod[1]; // TOD sec
    case 0x0A: return ci->tod[2]; // TOD min
    case 0x0B: return ci->tod[3]; // TOD hr
    case 0x0C: return ci->sdr;
    case 0x0D: { // ICR - read and clear
        uint8_t val = ci->icr;
        if (val & ci->icr_mask)
            val |= 0x80;
        ci->icr = 0;
        return val;
    }
    case 0x0E: return ci->cra;
    case 0x0F: return ci->crb;
    }
    return 0xFF;
}

static void cia_write(C64* c, int idx, uint8_t reg, uint8_t val) {
    CIA* ci = &c->cia[idx];
    switch (reg & 0x0F) {
    case 0x00:
        ci->pra = val;
        if (idx == 1) {
            // CIA2 PRA bits 0-1 select VIC bank
            uint8_t bank = (~val) & 0x03;
            c->vic_screen_base = (c->vic_screen_base & 0x3FFF) | (bank << 14);
        }
        break;
    case 0x01: ci->prb = val; break;
    case 0x02: ci->ddra = val; break;
    case 0x03: ci->ddrb = val; break;
    case 0x04: ci->latch_a = (ci->latch_a & 0xFF00) | val; break;
    case 0x05:
        ci->latch_a = (ci->latch_a & 0x00FF) | ((uint16_t)val << 8);
        if (!(ci->cra & 0x01)) // if timer stopped, load latch
            ci->timer_a = ci->latch_a;
        break;
    case 0x06: ci->latch_b = (ci->latch_b & 0xFF00) | val; break;
    case 0x07:
        ci->latch_b = (ci->latch_b & 0x00FF) | ((uint16_t)val << 8);
        if (!(ci->crb & 0x01))
            ci->timer_b = ci->latch_b;
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: {
        int i = (reg & 0x0F) - 0x08;
        if (ci->tod_write_alarm)
            ci->alarm[i] = val;
        else
            ci->tod[i] = val;
        break;
    }
    case 0x0C: ci->sdr = val; break;
    case 0x0D: // ICR mask
        if (val & 0x80)
            ci->icr_mask |= (val & 0x1F);
        else
            ci->icr_mask &= ~(val & 0x1F);
        break;
    case 0x0E:
        ci->cra = val;
        if (val & 0x10) { // force load
            ci->timer_a = ci->latch_a;
            ci->cra &= ~0x10;
        }
        break;
    case 0x0F:
        ci->crb = val;
        ci->tod_write_alarm = (val & 0x80) != 0;
        if (val & 0x10) {
            ci->timer_b = ci->latch_b;
            ci->crb &= ~0x10;
        }
        break;
    }
}

// =============================================================================
// Mapeo de memoria 6510
// =============================================================================

static uint8_t mem_read(void* userdata, uint16_t addr) {
    C64* c = (C64*)userdata;

    // $0000-$0001: 6510 I/O port
    if (addr == 0x0000) return c->port_ddr;
    if (addr == 0x0001) {
        // 6510 port: output bits from port_dat, input bits float high.
        // Bit assignments: 0=LORAM, 1=HIRAM, 2=CHAREN, 3=tape WR, 4=tape SENSE/READ, 5=tape motor
        uint8_t input_mask = ~c->port_ddr;
        uint8_t val = (c->port_dat & c->port_ddr) | (0xFF & input_mask);

        // Bit 4 (input when DDR bit4=0): cassette SENSE line.
        // Low when PLAY button is pressed, high when not.
        // Tape data goes through CIA1 FLAG, not through this bit.
        if (input_mask & 0x10) {
            if (c->tape.button)
                val &= ~0x10;  // PLAY pressed -> sense low
            else
                val |= 0x10;   // not pressed -> high
        }
        return val;
    }

    uint8_t bank = c->port_dat & 0x07;

    // $A000-$BFFF: BASIC ROM or RAM
    if (addr >= 0xA000 && addr < 0xC000) {
        if ((bank & 0x03) == 0x03) return c->basic_rom[addr - 0xA000];
        return c->ram[addr];
    }

    // $D000-$DFFF: I/O or Character ROM or RAM
    if (addr >= 0xD000 && addr < 0xE000) {
        if (bank == 0 || bank == 4) return c->ram[addr]; // all RAM
        if (!(bank & 0x04)) return c->char_rom[addr - 0xD000]; // CHAREN=0

        // I/O area
        if (addr < 0xD400) { // VIC-II
            uint8_t r = addr & 0x3F;
            if (r == 0x11) return (c->vic_regs[0x11] & 0x7F) | ((c->vic_raster >> 1) & 0x80);
            if (r == 0x12) return (uint8_t)(c->vic_raster & 0xFF);
            if (r == 0x19) { // IRQ flags
                uint8_t val = 0;
                if (c->vic_irq_raster) val |= 0x01;
                if (val & (c->vic_regs[0x1A] & 0x0F)) val |= 0x80;
                return val;
            }
            if (r == 0x1A) return c->vic_regs[0x1A]; // IRQ mask
            if (r == 0x1E || r == 0x1F) return 0; // sprite collision (simplified)
            if (r >= 0x20 && r <= 0x2E) return c->vic_regs[r];
            if (r < 0x20) return c->vic_regs[r];
            return 0xFF;
        }
        if (addr < 0xD800) { // SID
            return sid_read(c, addr & 0x1F);
        }
        if (addr < 0xDC00) { // Color RAM
            return c->color_ram[addr - 0xD800] | 0xF0;
        }
        if (addr < 0xDD00) { // CIA 1
            return cia_read(c, 0, (uint8_t)(addr & 0x0F));
        }
        if (addr < 0xDE00) { // CIA 2
            return cia_read(c, 1, (uint8_t)(addr & 0x0F));
        }
        return 0xFF; // I/O expansion
    }

    // $E000-$FFFF: KERNAL ROM or RAM
    if (addr >= 0xE000) {
        if (bank & 0x02) return c->kernal_rom[addr - 0xE000];
        return c->ram[addr];
    }

    return c->ram[addr];
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    C64* c = (C64*)userdata;

    if (addr == 0x0000) { c->port_ddr = val; return; }
    if (addr == 0x0001) {
        c->port_dat = val;
        c->tape.motor = !(val & 0x20); // bit 5: motor (0=on)
        return;
    }

    // Always write to RAM underneath ROMs
    c->ram[addr] = val;

    uint8_t bank = c->port_dat & 0x07;

    // I/O writes
    if (addr >= 0xD000 && addr < 0xE000 && (bank & 0x04) && bank != 0 && bank != 4) {
        if (addr < 0xD400) { // VIC-II
            uint8_t r = addr & 0x3F;
            c->vic_regs[r] = val;
            if (r == 0x11) {
                c->vic_raster_irq = (c->vic_raster_irq & 0x00FF) | ((uint16_t)(val & 0x80) << 1);
            } else if (r == 0x12) {
                c->vic_raster_irq = (c->vic_raster_irq & 0x0100) | val;
            } else if (r == 0x18) {
                // Memory pointers
                // bits 1-3: character base (× 0x800)
                // bits 4-7: screen base (× 0x400)
                uint8_t vic_bank = (~c->cia[1].pra) & 0x03;
                c->vic_char_base = (uint16_t)(vic_bank << 14) | (uint16_t)((val >> 1) & 0x07) * 0x800;
                c->vic_screen_base = (uint16_t)(vic_bank << 14) | (uint16_t)((val >> 4) & 0x0F) * 0x400;
                c->vic_bitmap_base = (uint16_t)(vic_bank << 14) | (uint16_t)((val >> 3) & 0x01) * 0x2000;
            } else if (r == 0x19) {
                // Acknowledge IRQ flags by writing 1
                if (val & 0x01) c->vic_irq_raster = false;
            } else if (r == 0x20) {
                c->vic_border_color = val & 0x0F;
            } else if (r >= 0x21 && r <= 0x24) {
                c->vic_bg_color[r - 0x21] = val & 0x0F;
            }
            return;
        }
        if (addr < 0xD800) { // SID
            sid_write(c, (uint8_t)(addr & 0x1F), val);
            return;
        }
        if (addr < 0xDC00) { // Color RAM
            c->color_ram[addr - 0xD800] = val & 0x0F;
            return;
        }
        if (addr < 0xDD00) { // CIA 1
            cia_write(c, 0, (uint8_t)(addr & 0x0F), val);
            return;
        }
        if (addr < 0xDE00) { // CIA 2
            cia_write(c, 1, (uint8_t)(addr & 0x0F), val);
            return;
        }
    }
}

// =============================================================================
// VIC-II - renderizado por scanline
// =============================================================================

static uint8_t vic_read_byte(C64* c, uint16_t addr) {
    addr &= 0x3FFF;
    uint8_t bank = (~c->cia[1].pra) & 0x03;
    uint16_t full_addr = (uint16_t)(bank << 14) | addr;

    // Banks 0 and 2: character ROM at $1000-$1FFF
    if ((bank == 0 || bank == 2) && addr >= 0x1000 && addr < 0x2000)
        return c->char_rom[addr - 0x1000];

    return c->ram[full_addr];
}

static void vic_render_line(C64* c) {
    int screen_y = (int)c->vic_raster - VIC_FIRST_VISIBLE_LINE;
    if (screen_y < 0 || screen_y >= C64_FULL_H) return;

    uint32_t* line = &c->framebuffer[screen_y * C64_FULL_W];
    uint32_t border = c64_palette[c->vic_border_color];

    int display_y = (int)c->vic_raster - VIC_FIRST_DISPLAY_LINE;
    bool in_display = (display_y >= 0 && display_y < C64_SCREEN_H);

    // Screen control
    bool den = (c->vic_regs[0x11] & 0x10) != 0;
    bool bmm = (c->vic_regs[0x11] & 0x20) != 0;
    bool ecm = (c->vic_regs[0x11] & 0x40) != 0;
    bool mcm = (c->vic_regs[0x16] & 0x10) != 0;
    uint8_t x_scroll = c->vic_regs[0x16] & 0x07;
    bool csel = (c->vic_regs[0x16] & 0x08) != 0;
    bool rsel = (c->vic_regs[0x11] & 0x08) != 0;

    int border_left  = csel ? C64_BORDER_H : C64_BORDER_H + 8;
    int border_right = csel ? (C64_BORDER_H + C64_SCREEN_W) : (C64_BORDER_H + C64_SCREEN_W - 8);
    int border_top   = rsel ? 0 : 4;
    int border_bot   = rsel ? C64_SCREEN_H : (C64_SCREEN_H - 4);

    if (!in_display || !den || display_y < border_top || display_y >= border_bot) {
        for (int x = 0; x < C64_FULL_W; x++)
            line[x] = border;
        return;
    }

    // Render display area
    int char_row = display_y / 8;
    int pixel_row = display_y & 7;

    // Get VIC bank and memory pointers
    uint16_t screen_offset = ((uint16_t)(c->vic_regs[0x18] >> 4) & 0x0F) * 0x400;
    uint16_t char_offset = ((uint16_t)(c->vic_regs[0x18] >> 1) & 0x07) * 0x800;
    uint16_t bitmap_offset = ((uint16_t)(c->vic_regs[0x18] >> 3) & 0x01) * 0x2000;

    for (int x = 0; x < C64_FULL_W; x++) {
        if (x < border_left || x >= border_right) {
            line[x] = border;
            continue;
        }

        int disp_x = x - C64_BORDER_H - x_scroll;
        if (disp_x < 0 || disp_x >= 320) {
            line[x] = border;
            continue;
        }

        int char_col = disp_x / 8;
        int pixel_col = disp_x & 7;

        uint16_t screen_addr = screen_offset + (uint16_t)(char_row * 40 + char_col);
        uint8_t screen_code = vic_read_byte(c, screen_addr);
        uint8_t color = c->color_ram[char_row * 40 + char_col];

        uint32_t pixel_color;

        if (bmm) {
            // Bitmap mode
            uint16_t bmp_addr = bitmap_offset + (uint16_t)(char_row * 40 + char_col) * 8 + (uint16_t)pixel_row;
            uint8_t bmp_data = vic_read_byte(c, bmp_addr);

            if (mcm) {
                // Multicolor bitmap
                uint8_t bits = (bmp_data >> (6 - (pixel_col & 0x06))) & 0x03;
                switch (bits) {
                case 0: pixel_color = c64_palette[c->vic_bg_color[0]]; break;
                case 1: pixel_color = c64_palette[(screen_code >> 4) & 0x0F]; break;
                case 2: pixel_color = c64_palette[screen_code & 0x0F]; break;
                default: pixel_color = c64_palette[color & 0x0F]; break;
                }
            } else {
                // Standard bitmap
                bool bit = (bmp_data >> (7 - pixel_col)) & 1;
                if (bit)
                    pixel_color = c64_palette[(screen_code >> 4) & 0x0F];
                else
                    pixel_color = c64_palette[screen_code & 0x0F];
            }
        } else if (ecm) {
            // Extended Color Mode
            uint16_t char_addr = char_offset + (uint16_t)(screen_code & 0x3F) * 8 + (uint16_t)pixel_row;
            uint8_t char_data = vic_read_byte(c, char_addr);
            bool bit = (char_data >> (7 - pixel_col)) & 1;
            uint8_t bg_idx = (screen_code >> 6) & 0x03;
            if (bit)
                pixel_color = c64_palette[color & 0x0F];
            else
                pixel_color = c64_palette[c->vic_bg_color[bg_idx]];
        } else if (mcm && (color & 0x08)) {
            // Multicolor text mode (only for chars with color bit 3 set)
            uint16_t char_addr = char_offset + (uint16_t)screen_code * 8 + (uint16_t)pixel_row;
            uint8_t char_data = vic_read_byte(c, char_addr);
            uint8_t bits = (char_data >> (6 - (pixel_col & 0x06))) & 0x03;
            switch (bits) {
            case 0: pixel_color = c64_palette[c->vic_bg_color[0]]; break;
            case 1: pixel_color = c64_palette[c->vic_bg_color[1]]; break;
            case 2: pixel_color = c64_palette[c->vic_bg_color[2]]; break;
            default: pixel_color = c64_palette[color & 0x07]; break;
            }
        } else {
            // Standard text mode
            uint16_t char_addr = char_offset + (uint16_t)screen_code * 8 + (uint16_t)pixel_row;
            uint8_t char_data = vic_read_byte(c, char_addr);
            bool bit = (char_data >> (7 - pixel_col)) & 1;
            if (bit)
                pixel_color = c64_palette[color & 0x0F];
            else
                pixel_color = c64_palette[c->vic_bg_color[0]];
        }

        line[x] = pixel_color;
    }
}

static void vic_render_sprites(C64* c) {
    for (int s = 7; s >= 0; s--) {
        if (!(c->vic_regs[0x15] & (1 << s))) continue;

        int sx = (int)c->vic_regs[s * 2] | ((c->vic_regs[0x10] & (1 << s)) ? 256 : 0);
        int sy = (int)c->vic_regs[s * 2 + 1];
        uint8_t color = c->vic_regs[0x27 + s] & 0x0F;
        bool multicolor = (c->vic_regs[0x1C] & (1 << s)) != 0;
        bool expand_x = (c->vic_regs[0x1D] & (1 << s)) != 0;
        bool expand_y = (c->vic_regs[0x17] & (1 << s)) != 0;
        bool priority = (c->vic_regs[0x1B] & (1 << s)) != 0;

        // Get sprite pointer
        uint16_t screen_off = ((uint16_t)(c->vic_regs[0x18] >> 4) & 0x0F) * 0x400;
        uint16_t ptr_addr = screen_off + 0x03F8 + (uint16_t)s;
        uint8_t pointer = vic_read_byte(c, ptr_addr);

        int sprite_h = expand_y ? 42 : 21;
        int sprite_w = expand_x ? 48 : 24;

        for (int line = 0; line < sprite_h; line++) {
            int screen_line = sy + line - VIC_FIRST_VISIBLE_LINE;
            if (screen_line < 0 || screen_line >= C64_FULL_H) continue;

            int data_line = expand_y ? (line / 2) : line;
            uint16_t data_addr = (uint16_t)pointer * 64 + (uint16_t)(data_line * 3);

            uint8_t data[3];
            data[0] = vic_read_byte(c, data_addr);
            data[1] = vic_read_byte(c, data_addr + 1);
            data[2] = vic_read_byte(c, data_addr + 2);

            for (int px = 0; px < sprite_w; px++) {
                int screen_x = sx + px - 24; // sprite coords start at 24
                int fb_x = screen_x - (VIC_FIRST_VISIBLE_CYCLE * 8) + C64_BORDER_H;
                if (fb_x < 0 || fb_x >= C64_FULL_W) continue;

                int data_px = expand_x ? (px / 2) : px;
                int byte_idx = data_px / 8;
                int bit_idx = 7 - (data_px & 7);

                uint32_t pixel_color = 0;
                bool draw = false;

                if (multicolor) {
                    int pair_bit = data_px & ~1;
                    int byte_i = pair_bit / 8;
                    int bit_i = 6 - (pair_bit & 6);
                    uint8_t bits = (data[byte_i] >> bit_i) & 0x03;
                    switch (bits) {
                    case 0: break; // transparent
                    case 1: pixel_color = c64_palette[c->vic_regs[0x25] & 0x0F]; draw = true; break;
                    case 2: pixel_color = c64_palette[color]; draw = true; break;
                    case 3: pixel_color = c64_palette[c->vic_regs[0x26] & 0x0F]; draw = true; break;
                    }
                } else {
                    if (data[byte_idx] & (1 << bit_idx)) {
                        pixel_color = c64_palette[color];
                        draw = true;
                    }
                }

                if (draw && !priority) {
                    c->framebuffer[screen_line * C64_FULL_W + fb_x] = pixel_color;
                }
            }
        }
    }
}

// =============================================================================
// TAP - reproductor de cinta
// =============================================================================

static int tap_load(C64* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 20) { fclose(f); return -1; }

    uint8_t header[20];
    size_t nr = fread(header, 1, 20, f);
    (void)nr;

    if (memcmp(header, "C64-TAPE-RAW", 12) != 0) {
        fclose(f);
        return -1;
    }

    c->tape.version = header[12];
    uint32_t data_size = (uint32_t)header[16] |
                         ((uint32_t)header[17] << 8) |
                         ((uint32_t)header[18] << 16) |
                         ((uint32_t)header[19] << 24);

    if (c->tape.data) free(c->tape.data);
    c->tape.data = (uint8_t*)malloc(data_size);
    if (!c->tape.data) { fclose(f); return -1; }

    nr = fread(c->tape.data, 1, data_size, f);
    fclose(f);

    c->tape.size = data_size;
    c->tape.pos = 0;
    c->tape.playing = false;
    c->tape.pulse_cycles = 0;
    c->tape.level = false;
    c->tape.button = false;
    return 0;
}

static void tap_update(C64* c, int cycles) {
    if (!c->tape.data || !c->tape.playing || !c->tape.motor) return;

    c->tape.pulse_cycles -= cycles;

    // Each TAP byte = one complete pulse (full cycle between falling edges).
    // Trigger CIA1 FLAG once per TAP byte so the KERNAL can measure pulse widths.
    while (c->tape.pulse_cycles <= 0 && c->tape.playing) {
        if (c->tape.pos >= c->tape.size) {
            c->tape.playing = false;
            return;
        }

        c->cia[0].icr |= 0x10; // CIA1 FLAG — tape data edge

        uint8_t b = c->tape.data[c->tape.pos++];
        if (b != 0) {
            c->tape.pulse_cycles += (int32_t)b * 8;
        } else if (c->tape.version == 0) {
            c->tape.pulse_cycles += 256 * 8;
        } else if (c->tape.pos + 3 <= c->tape.size) {
            int32_t len = (int32_t)c->tape.data[c->tape.pos]
                        | ((int32_t)c->tape.data[c->tape.pos+1] << 8)
                        | ((int32_t)c->tape.data[c->tape.pos+2] << 16);
            c->tape.pos += 3;
            c->tape.pulse_cycles += len;
        } else {
            c->tape.playing = false;
        }
    }
}

// =============================================================================
// D64 - soporte de disco mediante KERNAL trapping
// =============================================================================

static const int d64_sectors_per_track[36] = {
    0,
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21, // tracks 1-17
    19,19,19,19,19,19,19,                                  // tracks 18-24
    18,18,18,18,18,18,                                     // tracks 25-30
    17,17,17,17,17                                         // tracks 31-35
};

static int d64_track_offset(int track) {
    int offset = 0;
    for (int t = 1; t < track && t <= D64_MAX_TRACK; t++)
        offset += d64_sectors_per_track[t] * D64_SECTOR_SIZE;
    return offset;
}

static uint8_t* d64_get_sector(C64* c, int track, int sector) {
    if (!c->disk.loaded || track < 1 || track > D64_MAX_TRACK) return NULL;
    if (sector < 0 || sector >= d64_sectors_per_track[track]) return NULL;
    int offset = d64_track_offset(track) + sector * D64_SECTOR_SIZE;
    if ((uint32_t)(offset + D64_SECTOR_SIZE) > c->disk.size) return NULL;
    return c->disk.data + offset;
}

static int d64_load_file(C64* c, const char* filename, int namelen, uint16_t* load_addr, bool use_file_addr) {
    // Search directory (track 18, starting at sector 1)
    // D64 directory sector layout:
    //   byte 0   = next dir track  (0 = last sector)
    //   byte 1   = next dir sector
    //   bytes 2+ = 8 directory entries, each 32 bytes:
    //     [0]    = file type ($00=scratched, bit7=closed, bits0-2=type)
    //     [1]    = file start track
    //     [2]    = file start sector
    //     [3-18] = filename (padded with $A0)
    //     ...
    int dir_track = D64_BAM_TRACK;
    int dir_sector = 1;

    while (dir_track != 0) {
        uint8_t* sector = d64_get_sector(c, dir_track, dir_sector);
        if (!sector) return -1;

        for (int entry = 0; entry < 8; entry++) {
            uint8_t* e = sector + 2 + entry * 32; // entries start at byte 2

            // Skip scratched/empty entries (type==0 means scratched)
            if ((e[0] & 0x07) == 0) continue;

            // Compare filename (C64 names padded with $A0)
            if (namelen == 0) continue;
            bool match = true;
            for (int i = 0; i < namelen && i < 16; i++) {
                uint8_t rc = (uint8_t)filename[i];
                if (rc == '*') { match = true; break; } // wildcard stops here
                uint8_t fc = e[3 + i]; // filename at offset 3..18 within entry
                if (fc != rc) { match = false; break; }
            }
            if (!match) continue;

            // Found file: load it following the track/sector chain
            int file_track  = e[1];
            int file_sector = e[2];
            bool first_sector = true;
            uint16_t addr = 0;
            uint16_t ptr  = 0;

            while (file_track != 0) {
                uint8_t* data = d64_get_sector(c, file_track, file_sector);
                if (!data) return -1;

                int next_track  = data[0];
                int next_sector = data[1];
                // If next_track==0, next_sector is the index (1-based) of the last used byte
                int data_len = (next_track == 0) ? (next_sector - 1) : 254;

                if (first_sector) {
                    // First two payload bytes are the load address
                    addr = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
                    if (use_file_addr)
                        *load_addr = addr;
                    ptr = *load_addr;
                    // Payload starts at byte 4, length is data_len-2 (we consumed 2 for addr)
                    for (int i = 4; i < 2 + data_len; i++) {
                        if (ptr < 0xFFFF) c->ram[ptr++] = data[i];
                    }
                    first_sector = false;
                } else {
                    for (int i = 2; i < 2 + data_len; i++) {
                        if (ptr < 0xFFFF) c->ram[ptr++] = data[i];
                    }
                }

                file_track  = next_track;
                file_sector = next_sector;
            }

            return (int)(ptr - *load_addr);
        }

        // Follow directory chain
        dir_track  = sector[0];
        dir_sector = sector[1];
    }
    return -1; // not found
}

static void d64_kernal_trap(C64* c) {
    // Trap LOAD kernal routine at $FFD5.
    // On LOAD entry the CPU just did JSR $FFD5; registers at this point:
    //   $B7 = filename length
    //   $BB/$BC = filename address (lo/hi)
    //   $B8 = logical file number
    //   $B9 = secondary address (0=load to BASIC ptr, 1=load to file addr)
    //   $BA = device number (set by OPEN/LOAD statement)
    //   A   = 0 (LOAD), 1 (VERIFY)
    // We trap only device 8.

    if (c->cpu.pc != 0xFFD5 || !c->disk.loaded) return;
    if (c->cpu.a != 0) return; // only handle LOAD, not VERIFY

    // Device comes from $BA (last used device number), not from X register
    uint8_t device = c->ram[0xBA];
    if (device != 8) return;

    uint8_t namelen  = c->ram[0xB7];
    uint16_t nameaddr = (uint16_t)c->ram[0xBB] | ((uint16_t)c->ram[0xBC] << 8);

    char filename[17];
    int flen = (namelen < 16) ? namelen : 16;
    for (int i = 0; i < flen; i++)
        filename[i] = (char)c->ram[nameaddr + i];
    filename[flen] = 0;

    uint8_t  sa        = c->ram[0xB9]; // secondary address
    bool use_file_addr = (sa != 0);    // sa==1: use address embedded in file
    uint16_t load_addr = 0;

    if (!use_file_addr) {
        // Load to current BASIC pointer ($2D/$2E)
        load_addr = (uint16_t)c->ram[0x2D] | ((uint16_t)c->ram[0x2E] << 8);
    }

    int result;

    // LOAD"$",8 → build directory listing as a BASIC program
    if (flen == 1 && filename[0] == '$') {
        // Build a fake BASIC program at load_addr (or $0801 if sa==0)
        uint16_t addr = use_file_addr ? 0x0801 : load_addr;
        if (!use_file_addr) addr = 0x0801;
        uint8_t* p = c->ram + addr;
        uint8_t* base = p;

        // Helper: write a BASIC line
        // Format: link_lo link_hi line_lo line_hi text... $00
        // We'll fix up links after.
        #define EMIT(b) *p++ = (uint8_t)(b)
        #define EMIT_STR(s) do { const char* _s=(s); while(*_s) EMIT((uint8_t)*_s++); } while(0)

        // Read disk name from BAM sector (track 18, sector 0)
        uint8_t* bam = d64_get_sector(c, 18, 0);
        char disk_name[17] = "????????????????";
        char disk_id[6]    = "??";
        if (bam) {
            for (int i = 0; i < 16; i++) {
                uint8_t ch = bam[0x90 + i];
                disk_name[i] = (ch == 0xA0) ? ' ' : (char)ch;
            }
            disk_name[16] = 0;
            disk_id[0] = (char)bam[0xA2];
            disk_id[1] = (char)bam[0xA3];
            disk_id[2] = 0;
        }

        // Line 0: disk header  0 "DISKNAME" ID
        uint8_t* line_start = p;
        p += 2; // link placeholder
        EMIT(0x00); EMIT(0x00); // line number 0
        EMIT(0x12); // RVS ON (PETSCII reverse)
        EMIT('"');
        EMIT_STR(disk_name);
        EMIT('"');
        EMIT(' ');
        EMIT_STR(disk_id);
        EMIT(0x00);
        // fix link
        uint16_t next = (uint16_t)(addr + (p - base));
        line_start[0] = (uint8_t)(next & 0xFF);
        line_start[1] = (uint8_t)(next >> 8);

        // Directory entries
        int dir_track = 18, dir_sector = 1;
        while (dir_track != 0) {
            uint8_t* sec = d64_get_sector(c, dir_track, dir_sector);
            if (!sec) break;
            for (int entry = 0; entry < 8; entry++) {
                uint8_t* e = sec + 2 + entry * 32;
                if ((e[0] & 0x07) == 0) continue; // scratched

                // Blocks used (at offset 28-29 in entry)
                uint16_t blocks = (uint16_t)e[28] | ((uint16_t)e[29] << 8);

                // File type string
                static const char* ftypes[] = {"DEL","SEQ","PRG","USR","REL","???","???","???"};
                const char* ftype = ftypes[e[0] & 0x07];
                bool closed = (e[0] & 0x80) != 0;
                bool locked = (e[0] & 0x40) != 0;

                // Filename (strip $A0 padding)
                char fname[17];
                int fnlen = 0;
                for (int i = 0; i < 16; i++) {
                    if (e[3+i] == 0xA0) break;
                    fname[fnlen++] = (char)e[3+i];
                }
                fname[fnlen] = 0;

                line_start = p;
                p += 2; // link placeholder
                EMIT((uint8_t)(blocks & 0xFF));
                EMIT((uint8_t)(blocks >> 8));

                // Padding so filename column lines up
                char blk_str[6];
                snprintf(blk_str, sizeof(blk_str), "%u", blocks);
                int pad = 3 - (int)strlen(blk_str);
                for (int i = 0; i < pad; i++) EMIT(' ');

                EMIT('"');
                EMIT_STR(fname);
                EMIT('"');
                // pad to 16 chars
                for (int i = fnlen; i < 16; i++) EMIT(' ');
                EMIT(' ');
                if (!closed) EMIT('*');
                EMIT_STR(ftype);
                if (locked) EMIT('<');
                EMIT(0x00);

                next = (uint16_t)(addr + (p - base));
                line_start[0] = (uint8_t)(next & 0xFF);
                line_start[1] = (uint8_t)(next >> 8);
            }
            dir_track  = sec[0];
            dir_sector = sec[1];
        }

        // Blocks free line
        // Count free blocks from BAM
        int free_blocks = 0;
        if (bam) {
            for (int t = 1; t <= 35; t++) {
                if (t == 18) continue;
                free_blocks += bam[4 + (t-1)*4]; // byte 0 of each BAM entry = free blocks
            }
        }
        line_start = p;
        p += 2;
        EMIT((uint8_t)(free_blocks & 0xFF));
        EMIT((uint8_t)(free_blocks >> 8));
        EMIT_STR("BLOCKS FREE.");
        EMIT(0x00);
        // final link = 0000
        line_start[0] = 0x00;
        line_start[1] = 0x00;

        // End of program
        EMIT(0x00); EMIT(0x00);

        #undef EMIT
        #undef EMIT_STR

        uint16_t end_addr = (uint16_t)(addr + (p - base));
        c->ram[0x2D] = (uint8_t)(end_addr & 0xFF);
        c->ram[0x2E] = (uint8_t)(end_addr >> 8);
        c->ram[0xAE] = c->ram[0x2D];
        c->ram[0xAF] = c->ram[0x2E];
        c->cpu.x  = c->ram[0xAE];
        c->cpu.y  = c->ram[0xAF];
        c->cpu.cf = 0;
        c->cpu.a  = 0;
        uint8_t rlo = c->ram[0x0100 + (uint8_t)(c->cpu.sp + 1)];
        uint8_t rhi = c->ram[0x0100 + (uint8_t)(c->cpu.sp + 2)];
        c->cpu.sp += 2;
        c->cpu.pc  = (((uint16_t)rhi << 8) | rlo) + 1;
        printf("D64 DIR OK: %d bytes\n", (int)(end_addr - addr));
        return;
    }

    result = d64_load_file(c, filename, flen, &load_addr, use_file_addr);

    if (result >= 0) {
        // Success: update end-of-load pointers and return with carry clear
        uint16_t end_addr = load_addr + (uint16_t)result;
        c->ram[0xAE] = (uint8_t)(end_addr & 0xFF);
        c->ram[0xAF] = (uint8_t)(end_addr >> 8);
        c->cpu.x  = c->ram[0xAE];
        c->cpu.y  = c->ram[0xAF];
        c->cpu.cf = 0; // no error
        c->cpu.a  = 0;
        printf("D64 LOAD \"%s\" OK: $%04X-$%04X (%d bytes)\n",
               filename, load_addr, end_addr, result);
    } else {
        // File not found: set carry and error code $04 (file not found)
        c->cpu.cf = 1;
        c->cpu.a  = 4;
        c->ram[0x90] = 0x42; // ST: file not found
        printf("D64 LOAD \"%s\" not found\n", filename);
    }

    // Skip the real KERNAL LOAD routine by popping the return address
    // (JSR pushed PC-1, so the return address on stack is the instruction after JSR)
    uint8_t lo = c->ram[0x0100 + (uint8_t)(c->cpu.sp + 1)];
    uint8_t hi = c->ram[0x0100 + (uint8_t)(c->cpu.sp + 2)];
    c->cpu.sp += 2;
    c->cpu.pc = (((uint16_t)hi << 8) | lo) + 1; // RTS semantics: +1
}

/* ============================================================
   Utilidades LE
   ============================================================ */
static inline uint16_t rd16le(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* ============================================================
   CARGA T64 (contenedor de PRG)
   Formato del directorio T64 (cada entrada, 32 bytes, desde offset 0x40):
     +0x00  1  tipo de entrada: 0=vacía, 1=stream normal, 3=snapshot
     +0x01  1  tipo C64: 0x82=PRG, 0x81=SEQ, etc.
     +0x02  2  dirección de inicio (LE)  ← load address real
     +0x04  2  dirección de fin   (LE)   ← end address (exclusive)
     +0x06  2  reservado
     +0x08  4  offset dentro del fichero T64 hacia los datos del PRG (LE)
     +0x0C  4  reservado
     +0x10 16  nombre en PETSCII (relleno con 0x20 o 0xA0)
   Los datos del PRG en el fichero NO llevan cabecera de 2 bytes:
   la dirección de carga ya está en e[0x02-0x03] del directorio.
   ============================================================ */
int c64_load_t64(C64* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* Cabecera mínima: 32 bytes de tape record + 32 bytes de una entrada */
    if (sz < 64) { fclose(f); return -1; }

    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    /* Validar firma T64 — los primeros 32 bytes son texto libre, pero
       los ficheros bien formados empiezan con "C64" o "C64S". */
    if (memcmp(buf, "C64", 3) != 0) {
        free(buf);
        return -1;
    }

    /* Cabecera del tape record (offsets dentro del bloque de 32 bytes):
         0x20  2  número máximo de entradas en el directorio
         0x22  2  número de entradas usadas                        */
    uint16_t max_entries  = rd16le(buf + 0x20);
    uint16_t used_entries = rd16le(buf + 0x22);

    /* Protección: si el fichero declara 0 entradas máximas, es corrupto;
       usamos un límite razonable calculado a partir del tamaño del fichero. */
    if (max_entries == 0 || max_entries > 8192)
        max_entries = (uint16_t)((sz - 0x40) / 32);
    /* used_entries puede estar en 0 en ficheros mal generados; en ese caso
       iteramos hasta max_entries. */
    uint16_t limit = (used_entries > 0 && used_entries <= max_entries)
                     ? used_entries : max_entries;

    const uint32_t dir_off = 0x40;
    int found_idx = -1;

    for (int i = 0; i < (int)limit; i++) {
        /* Comprobamos que la entrada cabe en el buffer */
        if (dir_off + (uint32_t)(i + 1) * 32 > (uint32_t)sz) break;

        const uint8_t* e = buf + dir_off + (uint32_t)i * 32;

        /* BUG 1 CORREGIDO: el tipo de entrada está en e[0], no en e[1].
           e[0]==0 → entrada vacía; e[0]==1 → entrada normal (PRG/SEQ/…).
           e[1] es el tipo C64 (0x82=PRG cerrado, 0x01=PRG de snapshot, …).
           Aceptamos entradas con e[0] != 0 y e[1] conocido. */
        if (e[0] == 0) continue;                  /* vacía */
        uint8_t c64_type = e[1];
        if (c64_type == 0) continue;              /* sin tipo válido */

        /* BUG 2 CORREGIDO: la dirección de carga está en el directorio
           (e+0x02), NO en los primeros 2 bytes de los datos del fichero.
           Los datos del PRG en el T64 son contenido puro, sin cabecera. */
        uint16_t load_addr      = rd16le(e + 0x02);
        uint16_t end_addr_entry = rd16le(e + 0x04);  /* exclusivo */

        /* BUG 3 CORREGIDO: el offset de datos está en e+0x08 (4 bytes LE).
           Antes se leía con rd32le(e + 8) pero el índice era correcto;
           lo que fallaba era asumir que buf[data_off] era una cabecera PRG
           de 2 bytes y saltarla. Aquí NO saltamos nada. */
        uint32_t data_off = rd32le(e + 0x08);

        /* Validar que el offset apunta dentro del fichero */
        if (data_off == 0 || data_off >= (uint32_t)sz) continue;

        /* BUG 4 CORREGIDO: la longitud se calcula como end_addr - load_addr,
           NO como end_addr - start_addr_del_directorio otra vez.
           Además, en algunos T64 end_addr == 0 o end_addr <= load_addr
           (ficheros mal generados); en ese caso calculamos la longitud
           desde los datos disponibles en el fichero. */
        uint32_t prg_len;
        if (end_addr_entry > load_addr) {
            prg_len = (uint32_t)(end_addr_entry - load_addr);
        } else {
            /* Fallback: usar los bytes disponibles desde data_off hasta el
               final del fichero (típico en snaps o T64 mal escritos). */
            prg_len = (uint32_t)sz - data_off;
        }

        /* Asegurarnos de no desbordarnos ni del buffer T64 ni de la RAM */
        if (data_off + prg_len > (uint32_t)sz)
            prg_len = (uint32_t)sz - data_off;
        if ((uint32_t)load_addr + prg_len > 65536)
            prg_len = 65536 - load_addr;
        if (prg_len == 0) continue;

        /* Copiar el PRG a la RAM del C64 */
        memcpy(c->ram + load_addr, buf + data_off, prg_len);

        uint16_t real_end = (uint16_t)(load_addr + prg_len);

        /* Extraer nombre PETSCII → ASCII imprimible para el log */
        char name[17] = {0};
        for (int j = 0; j < 16; j++) {
            uint8_t ch = e[0x10 + j];
            if (ch == 0x00 || ch == 0xA0 || ch == 0x20) break;
            /* Convertir PETSCII mayúsculas (0x41-0x5A) a ASCII */
            if (ch >= 0x41 && ch <= 0x5A) ch = (uint8_t)(ch - 0x41 + 'A');
            name[j] = (char)ch;
        }

        /* Actualizar punteros del sistema BASIC */
        c->ram[0xAE] = (uint8_t)(real_end & 0xFF);
        c->ram[0xAF] = (uint8_t)(real_end >> 8);

        if (load_addr == 0x0801) {
            /* Fin de programa BASIC y punteros de variables */
            c->ram[0x2D] = c->ram[0xAE];
            c->ram[0x2E] = c->ram[0xAF];
            c->ram[0x2F] = c->ram[0xAE]; c->ram[0x30] = c->ram[0xAF]; /* VARTAB */
            c->ram[0x31] = c->ram[0xAE]; c->ram[0x32] = c->ram[0xAF]; /* ARYTAB */
            c->ram[0x33] = c->ram[0xAE]; c->ram[0x34] = c->ram[0xAF]; /* STREND */
        }

        printf("T64: '%s' cargado en $%04X-$%04X (%u bytes)\n",
               name, load_addr, real_end, prg_len);
        found_idx = i;
        break; /* cargamos sólo el primer archivo encontrado */
    }

    free(buf);
    return (found_idx >= 0) ? 0 : -1;
}

// =============================================================================
// PRG - carga directa de programas
// =============================================================================

int c64_load_prg(C64* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 3) { fclose(f); return -1; }

    uint8_t hdr[2];
    size_t nr = fread(hdr, 1, 2, f);
    (void)nr;
    uint16_t addr = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);

    nr = fread(c->ram + addr, 1, (size_t)(sz - 2), f);
    fclose(f);

    // If loaded to BASIC area, update BASIC pointers
    if (addr == 0x0801) {
        uint16_t end = addr + (uint16_t)(sz - 2);
        c->ram[0x2D] = (uint8_t)(end & 0xFF);
        c->ram[0x2E] = (uint8_t)(end >> 8);
        c->ram[0x2F] = c->ram[0x2D];
        c->ram[0x30] = c->ram[0x2E];
        c->ram[0x31] = c->ram[0x2D];
        c->ram[0x32] = c->ram[0x2E];
        c->ram[0xAE] = c->ram[0x2D];
        c->ram[0xAF] = c->ram[0x2E];
    }

    printf("PRG cargado en $%04X-$%04X (%ld bytes)\n", addr, addr + (int)(sz - 2), sz - 2);
    return 0;
}

// =============================================================================
// Carga de ROMs
// =============================================================================

static int load_rom_file(const char* dir, const char* name, uint8_t* dst, int size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t nr = fread(dst, 1, (size_t)size, f);
    (void)nr;
    fclose(f);
    return 0;
}

int c64_load_roms(C64* c, const char* dir) {
    static const char* basic_names[] = { "basic", "basic.901226-01.bin", NULL };
    static const char* kernal_names[] = { "kernal", "kernal.901227-03.bin", NULL };
    static const char* chargen_names[] = { "chargen", "characters.901225-01.bin", NULL };

    int ok = 0;
    bool found;

    // BASIC ROM
    found = false;
    for (int i = 0; basic_names[i]; i++) {
        if (load_rom_file(dir, basic_names[i], c->basic_rom, 8192) == 0) {
            found = true; break;
        }
    }
    if (!found) { fprintf(stderr, "Error: BASIC ROM not found\n"); ok = -1; }

    // KERNAL ROM
    found = false;
    for (int i = 0; kernal_names[i]; i++) {
        if (load_rom_file(dir, kernal_names[i], c->kernal_rom, 8192) == 0) {
            found = true; break;
        }
    }
    if (!found) { fprintf(stderr, "Error: KERNAL ROM not found\n"); ok = -1; }

    // Character ROM
    found = false;
    for (int i = 0; chargen_names[i]; i++) {
        if (load_rom_file(dir, chargen_names[i], c->char_rom, 4096) == 0) {
            found = true; break;
        }
    }
    if (!found) { fprintf(stderr, "Error: CHARGEN ROM not found\n"); ok = -1; }

    return ok;
}

// =============================================================================
// Carga de D64
// =============================================================================

int c64_load_d64(C64* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != D64_SIZE_35 && sz != D64_SIZE_35E) {
        fprintf(stderr, "D64: tamaño inesperado (%ld bytes)\n", sz);
        fclose(f);
        return -1;
    }

    if (c->disk.data) free(c->disk.data);
    c->disk.data = (uint8_t*)malloc((size_t)sz);
    if (!c->disk.data) { fclose(f); return -1; }

    size_t nr = fread(c->disk.data, 1, (size_t)sz, f);
    (void)nr;
    fclose(f);

    c->disk.size = (uint32_t)sz;
    c->disk.loaded = true;
    return 0;
}

// =============================================================================
// Carga de TAP
// =============================================================================

int c64_load_tap(C64* c, const char* path) {
    return tap_load(c, path);
}

// =============================================================================
// Teclado C64 - mapeo SDL → matriz 8×8
// =============================================================================

typedef struct {
    SDL_Scancode key;
    int col, row;
} C64KeyMap;

static const C64KeyMap keymap[] = {
    // Row 0: DEL, RETURN, RIGHT, F7, F1, F3, F5, DOWN
    { SDL_SCANCODE_BACKSPACE,    0, 0 },
    { SDL_SCANCODE_RETURN,       0, 1 },
    { SDL_SCANCODE_RIGHT,        0, 2 }, // also joystick
    { SDL_SCANCODE_F7,           0, 3 },
    { SDL_SCANCODE_F1,           0, 4 },
    { SDL_SCANCODE_F3,           0, 5 },
    { SDL_SCANCODE_F5,           0, 6 },
    { SDL_SCANCODE_DOWN,         0, 7 }, // also joystick
    // Row 1: 3, W, A, 4, Z, S, E, LSHIFT
    { SDL_SCANCODE_3,            1, 0 },
    { SDL_SCANCODE_W,            1, 1 },
    { SDL_SCANCODE_A,            1, 2 },
    { SDL_SCANCODE_4,            1, 3 },
    { SDL_SCANCODE_Z,            1, 4 },
    { SDL_SCANCODE_S,            1, 5 },
    { SDL_SCANCODE_E,            1, 6 },
    { SDL_SCANCODE_LSHIFT,       1, 7 },
    // Row 2: 5, R, D, 6, C, F, T, X
    { SDL_SCANCODE_5,            2, 0 },
    { SDL_SCANCODE_R,            2, 1 },
    { SDL_SCANCODE_D,            2, 2 },
    { SDL_SCANCODE_6,            2, 3 },
    { SDL_SCANCODE_C,            2, 4 },
    { SDL_SCANCODE_F,            2, 5 },
    { SDL_SCANCODE_T,            2, 6 },
    { SDL_SCANCODE_X,            2, 7 },
    // Row 3: 7, Y, G, 8, B, H, U, V
    { SDL_SCANCODE_7,            3, 0 },
    { SDL_SCANCODE_Y,            3, 1 },
    { SDL_SCANCODE_G,            3, 2 },
    { SDL_SCANCODE_8,            3, 3 },
    { SDL_SCANCODE_B,            3, 4 },
    { SDL_SCANCODE_H,            3, 5 },
    { SDL_SCANCODE_U,            3, 6 },
    { SDL_SCANCODE_V,            3, 7 },
    // Row 4: 9, I, J, 0, M, K, O, N
    { SDL_SCANCODE_9,            4, 0 },
    { SDL_SCANCODE_I,            4, 1 },
    { SDL_SCANCODE_J,            4, 2 },
    { SDL_SCANCODE_0,            4, 3 },
    { SDL_SCANCODE_M,            4, 4 },
    { SDL_SCANCODE_K,            4, 5 },
    { SDL_SCANCODE_O,            4, 6 },
    { SDL_SCANCODE_N,            4, 7 },
    // Row 5: +, P, L, -, ., :, @, ,
    { SDL_SCANCODE_EQUALS,       5, 0 }, // + (shift handled externally)
    { SDL_SCANCODE_P,            5, 1 },
    { SDL_SCANCODE_L,            5, 2 },
    { SDL_SCANCODE_MINUS,        5, 3 },
    { SDL_SCANCODE_PERIOD,       5, 4 },
    { SDL_SCANCODE_SEMICOLON,    5, 5 }, // :
    { SDL_SCANCODE_LEFTBRACKET,  5, 6 }, // @
    { SDL_SCANCODE_COMMA,        5, 7 },
    // Row 6: £, *, ;, HOME, RSHIFT, =, ↑, /
    { SDL_SCANCODE_BACKSLASH,    6, 0 }, // £
    { SDL_SCANCODE_RIGHTBRACKET, 6, 1 }, // *
    { SDL_SCANCODE_APOSTROPHE,   6, 2 }, // ;
    { SDL_SCANCODE_HOME,         6, 3 },
    { SDL_SCANCODE_RSHIFT,       6, 4 },
    { SDL_SCANCODE_END,          6, 5 }, // =
    { SDL_SCANCODE_GRAVE,        6, 6 }, // ↑
    { SDL_SCANCODE_SLASH,        6, 7 },
    // Row 7: 1, ←, CTRL, 2, SPACE, C=, Q, STOP
    { SDL_SCANCODE_1,            7, 0 },
    { SDL_SCANCODE_ESCAPE,       7, 1 }, // ← (left arrow)
    { SDL_SCANCODE_TAB,          7, 2 }, // CTRL
    { SDL_SCANCODE_2,            7, 3 },
    { SDL_SCANCODE_SPACE,        7, 4 },
    { SDL_SCANCODE_LCTRL,        7, 5 }, // C= (Commodore key)
    { SDL_SCANCODE_Q,            7, 6 },
    { SDL_SCANCODE_RCTRL,        7, 7 }, // RUN/STOP
    { 0, -1, -1 } // sentinel
};

static void c64_handle_key(C64* c, SDL_Scancode sc, bool pressed) {
    // F2 = turbo mode toggle
    if (sc == SDL_SCANCODE_F2 && pressed) {
        c->turbo_mode = !c->turbo_mode;
        if (!c->turbo_mode && c->audio_dev)
            SDL_ClearQueuedAudio(c->audio_dev);
        return;
    }

    // F9 = tape play/stop
    if (sc == SDL_SCANCODE_F9 && pressed) {
        if (c->tape.data) {
            c->tape.playing = !c->tape.playing;
            c->tape.button = c->tape.playing;
            if (c->tape.playing) {
                c->tape.pos = 0;
                c->tape.pulse_cycles = 0;
                c->tape.level = false;
            }
        }
        return;
    }

    // Joystick mapping (numpad or arrow alternatives when LALT held)
    // Using numpad: 8=up, 2=down, 4=left, 6=right, 5=fire
    switch (sc) {
    case SDL_SCANCODE_KP_8: if (pressed) c->joystick &= ~0x01; else c->joystick |= 0x01; return;
    case SDL_SCANCODE_KP_2: if (pressed) c->joystick &= ~0x02; else c->joystick |= 0x02; return;
    case SDL_SCANCODE_KP_4: if (pressed) c->joystick &= ~0x04; else c->joystick |= 0x04; return;
    case SDL_SCANCODE_KP_6: if (pressed) c->joystick &= ~0x08; else c->joystick |= 0x08; return;
    case SDL_SCANCODE_KP_5:
    case SDL_SCANCODE_KP_0: if (pressed) c->joystick &= ~0x10; else c->joystick |= 0x10; return;
    default: break;
    }

    // Keyboard matrix
    for (int i = 0; keymap[i].row >= 0; i++) {
        if (keymap[i].key == sc) {
            if (pressed)
                c->keyboard_matrix[keymap[i].row] &= ~(1 << keymap[i].col);
            else
                c->keyboard_matrix[keymap[i].row] |= (1 << keymap[i].col);
            return;
        }
    }
}

// =============================================================================
// Inicialización / destrucción
// =============================================================================

void c64_init(C64* c) {
    memset(c, 0, sizeof(C64));

    // Keyboard matrix: all keys released (1 = not pressed)
    memset(c->keyboard_matrix, 0xFF, sizeof(c->keyboard_matrix));
    c->joystick = 0xFF;

    // 6510 port
    c->port_ddr = 0x2F;
    c->port_dat = 0x37;

    // VIC-II defaults
    memset(c->vic_regs, 0, sizeof(c->vic_regs));
    c->vic_regs[0x11] = 0x1B; // standard text mode, 25 rows, DEN on
    c->vic_regs[0x16] = 0xC8; // 40 columns, no MCM
    c->vic_regs[0x18] = 0x14; // screen at $0400, chars at $1000
    c->vic_border_color = 14;  // light blue
    c->vic_bg_color[0] = 6;    // blue

    // CIA reset
    cia_reset(&c->cia[0]);
    cia_reset(&c->cia[1]);

    // SID reset
    sid_reset(c);

    // CPU init
    m6502_init(&c->cpu);
    c->cpu.read_byte = mem_read;
    c->cpu.write_byte = mem_write;
    c->cpu.userdata = c;
    c->cpu.enable_bcd = true;

    // SDL video
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return;
    }

    c->window = SDL_CreateWindow("C64",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        C64_FULL_W * C64_SCALE, C64_FULL_H * C64_SCALE,
        SDL_WINDOW_SHOWN);

    c->renderer = SDL_CreateRenderer(c->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    c->texture = SDL_CreateTexture(c->renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        C64_FULL_W, C64_FULL_H);

    // SDL audio
    SDL_AudioSpec want = {0}, have;
    want.freq = C64_AUDIO_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    c->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (c->audio_dev > 0)
        SDL_PauseAudioDevice(c->audio_dev, 0);
}

void c64_destroy(C64* c) {
    if (c->audio_dev > 0) SDL_CloseAudioDevice(c->audio_dev);
    if (c->texture) SDL_DestroyTexture(c->texture);
    if (c->renderer) SDL_DestroyRenderer(c->renderer);
    if (c->window) SDL_DestroyWindow(c->window);
    if (c->tape.data) free(c->tape.data);
    if (c->disk.data) free(c->disk.data);
    SDL_Quit();
}

// =============================================================================
// Frame principal
// =============================================================================

void c64_run_frame(C64* c) {
    
    int cycles_done = 0;
    float sample_accum = (float)C64_CPU_FREQ / (float)C64_AUDIO_RATE;
    float next_sample_at = 0.0f;

    c->audio_pos = 0;

    while (cycles_done < C64_CYCLES_PER_FRAME) {

		// Deferred PRG load: wait for BASIC to finish initialization
		if (c->prg_pending && (c->cpu.pc == 0xa65c) /* BASIC end loading */ ) {
			c64_load_prg(c, c->pending_prg);
			c->prg_pending = false;
		}

		// Deferred T64 load: same delay so BASIC vectors are ready
		if (c->t64_pending && (c->cpu.pc == 0xa65c) /* BASIC end loading */ ) {
			c64_load_t64(c, c->pending_t64);
			c->t64_pending = false;
		}

        // KERNAL trap for D64 loading
        if (c->disk.loaded)
            d64_kernal_trap(c);

        // Execute one CPU instruction
        unsigned long cyc_before = c->cpu.cyc;
        m6502_step(&c->cpu);
        int elapsed = (int)(c->cpu.cyc - cyc_before);

        // Update CIAs
        cia_step(&c->cia[0], elapsed);
        cia_step(&c->cia[1], elapsed);

        // Tape update - must happen before CIA IRQ check so FLAG events are seen immediately
        tap_update(c, elapsed);

        // CIA1 IRQ -> CPU IRQ (timers + FLAG from tape)
        if (c->cia[0].icr & c->cia[0].icr_mask)
            m6502_gen_irq(&c->cpu);

        // CIA2 IRQ -> CPU NMI
        if (c->cia[1].icr & c->cia[1].icr_mask)
            m6502_gen_nmi(&c->cpu);

        // VIC-II raster advance
        c->vic_cycle += elapsed;
        while (c->vic_cycle >= C64_CYCLES_PER_LINE) {
            c->vic_cycle -= C64_CYCLES_PER_LINE;

            // Render current line
            vic_render_line(c);
            vic_render_sprites(c);

            c->vic_raster++;
            if (c->vic_raster >= C64_LINES_PER_FRAME) {
                c->vic_raster = 0;
            }

            // Raster IRQ
            if (c->vic_raster == c->vic_raster_irq) {
                c->vic_irq_raster = true;
                if (c->vic_regs[0x1A] & 0x01) // raster IRQ enabled
                    m6502_gen_irq(&c->cpu);
            }
        }

        // Audio sampling
        float pos_f = (float)cycles_done;
        while (pos_f >= next_sample_at) {
            if (c->audio_pos < C64_SAMPLES_PER_FRAME * 2 && !c->turbo_mode) {
                c->audio_buffer[c->audio_pos++] = sid_generate_sample(c);
            }
            next_sample_at += sample_accum;
        }

        cycles_done += elapsed;
    }

    // Queue audio
    if (c->audio_dev > 0 && c->audio_pos > 0 && !c->turbo_mode) {
        SDL_QueueAudio(c->audio_dev, c->audio_buffer, (uint32_t)(c->audio_pos) * sizeof(float));
    }
    c->audio_pos = 0;
    c->frame_counter++;
}

// =============================================================================
// Render SDL
// =============================================================================

void c64_render(C64* c) {
    SDL_UpdateTexture(c->texture, NULL, c->framebuffer, C64_FULL_W * sizeof(uint32_t));
    SDL_RenderClear(c->renderer);
    SDL_Rect dst = { 0, 0, C64_FULL_W * C64_SCALE, C64_FULL_H * C64_SCALE };
    SDL_RenderCopy(c->renderer, c->texture, NULL, &dst);
    SDL_RenderPresent(c->renderer);
}

// =============================================================================
// Utilidades
// =============================================================================

static bool ext_eq(const char* path, const char* ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char* tail = path + plen - elen;
    for (size_t i = 0; i < elen; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    c64_init(&c64);

    // Load ROMs from current directory or specified path
    const char* rom_dir = ".";
    if (argc > 1 && !ext_eq(argv[1], ".prg") && !ext_eq(argv[1], ".d64") && !ext_eq(argv[1], ".tap") && !ext_eq(argv[1], ".t64"))
        rom_dir = argv[1];

    if (c64_load_roms(&c64, rom_dir) != 0) {
        fprintf(stderr, "ROMs necesarias: basic, kernal, chargen\n");
        c64_destroy(&c64);
        return 1;
    }

    // Reset CPU (read reset vector)
    m6502_gen_res(&c64.cpu);

    // Load files from command line
    for (int i = 1; i < argc; i++) {
        if (ext_eq(argv[i], ".prg")) {
            strncpy(c64.pending_prg, argv[i], sizeof(c64.pending_prg) - 1);
            c64.pending_prg[sizeof(c64.pending_prg) - 1] = '\0';
            c64.prg_pending = true;
            printf("PRG pendiente. Escribe RUN para ejecutar.\n");
        } else if (ext_eq(argv[i], ".d64")) {
            if (c64_load_d64(&c64, argv[i]) == 0)
                printf("D64 montado: %s\n  LOAD\"$\",8 para directorio, LOAD\"*\",8,1 para primer programa\n", argv[i]);
        } else if (ext_eq(argv[i], ".tap")) {
            if (c64_load_tap(&c64, argv[i]) == 0)
                printf("TAP cargado: %s\n  LOAD y pulsa F9 para reproducir\n", argv[i]);
        }  else if (ext_eq(argv[i], ".t64")) {
			strncpy(c64.pending_t64, argv[i], sizeof(c64.pending_t64) - 1);
            c64.pending_t64[sizeof(c64.pending_t64) - 1] = '\0';
            c64.t64_pending = true;
            printf("T64 pendiente: %s\n  Se cargará automáticamente al arrancar.\n", argv[i]);
        }
    }

    if (argc <= 1) {
        printf("Uso: %s [rom_dir] [archivo.prg|.d64|.tap|.t64]\n", argv[0]);
        printf("  ROMs necesarias en rom_dir: basic, kernal, chargen\n");
        printf("  F2  = velocidad maxima / normal\n");
        printf("  F9  = play/stop cinta\n");
        printf("  Numpad: 8/2/4/6 = joystick, 5/0 = fire\n");
        printf("  Tab = CTRL, LCtrl = C=, Esc = left arrow, RCtrl = RUN/STOP\n");
    }

    const uint32_t FRAME_MS = 20; // 50 Hz PAL

    while (!c64.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                c64.quit = true;
            else if (e.type == SDL_KEYDOWN)
                c64_handle_key(&c64, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)
                c64_handle_key(&c64, e.key.keysym.scancode, false);
        }

        c64_run_frame(&c64);

        if (c64.turbo_mode) {
            if ((c64.frame_counter & 7) == 0)
                c64_render(&c64);
        } else {
            c64_render(&c64);
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < FRAME_MS)
                SDL_Delay(FRAME_MS - elapsed);
        }
    }

    c64_destroy(&c64);
    return 0;
}