/*
 * msx.c  –  MSX1 & MSX2 emulator core usando tms9918.c para MSX1
 *
 * MSX1: TMS9918A completo vía tms9918.c
 * MSX2: V9938 básico (mantenido por compatibilidad)
 */

#include "msx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>

extern SDL_Renderer*     msx_renderer;
extern SDL_Texture*      msx_texture;
extern SDL_AudioDeviceID msx_audio_dev;

uint32_t msx_pixels[MSX_H * MSX_W];

static int try_load(const char* path, uint8_t* dst, size_t sz);

/* ────────────────────────────────────────────────────────────────────────── */
/* Modelo                                                                     */
/* ────────────────────────────────────────────────────────────────────────── */
static MSXModel msx_model = MSX_MODEL_1;

/* ────────────────────────────────────────────────────────────────────────── */
/* Slots / Memoria                                                            */
/* ────────────────────────────────────────────────────────────────────────── */
#define PAGE_SIZE 0x4000

static uint8_t rom_bios [0x8000];   // 32KB
static uint8_t rom_basic[0x4000];   // 16KB
static uint8_t rom_ext  [0x4000];   // 16KB (MSX2)
static uint8_t ram_main [0x10000];  // 64KB

// MSX2: RAM mapper 256KB
#define RAM_BANKS_MSX2 16
static uint8_t ram_extra[RAM_BANKS_MSX2][PAGE_SIZE];
static uint8_t ram_mapper[4] = {3,2,1,0};

static uint8_t slot_select = 0;
static uint8_t subslot_reg = 0;

static inline int slot_for_page(int page) {
    return (slot_select >> (page * 2)) & 3;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Cartuchos / Mappers                                                        */
/* ────────────────────────────────────────────────────────────────────────── */
#define CART_MAX_SIZE 0x80000u
static uint8_t  cart_rom[CART_MAX_SIZE];
static uint32_t cart_size = 0;

typedef enum {
    CART_MAPPER_NONE = 0,
    CART_MAPPER_ASCII8,
    CART_MAPPER_ASCII16,
    CART_MAPPER_KONAMI8
} CartMapper;

static CartMapper cart_mapper = CART_MAPPER_NONE;
static uint8_t  cart_bank8[4] = {0,1,2,3};
static uint32_t cart_mask8 = 0;

static inline uint32_t cart_num_banks8(void) {
    return (cart_size + 0x1FFFu) / 0x2000u;
}

static void cart_reset_banks(void) {
    cart_bank8[0] = 0; cart_bank8[1] = 1; cart_bank8[2] = 2; cart_bank8[3] = 3;
    uint32_t nb = cart_num_banks8();
    cart_mask8 = (nb && ((nb & (nb-1)) == 0)) ? (nb - 1) : 0;
}

static CartMapper detect_mapper_by_size(uint32_t sz) {
    if (sz <= 0x8000u) return CART_MAPPER_NONE;
    if (sz == 0x10000u) return CART_MAPPER_ASCII16;
    return CART_MAPPER_ASCII8;
}

static void cart_mapper_write(uint16_t addr, uint8_t val) {
    if (!cart_size) return;

    switch (cart_mapper) {
        case CART_MAPPER_ASCII8:
            if ((addr & 0xF800) == 0x6000) cart_bank8[0] = val;
            else if ((addr & 0xF800) == 0x6800) cart_bank8[1] = val;
            else if ((addr & 0xF800) == 0x7000) cart_bank8[2] = val;
            else if ((addr & 0xF800) == 0x7800) cart_bank8[3] = val;
            break;

        case CART_MAPPER_ASCII16:
            if ((addr & 0xF000) == 0x6000) {
                uint8_t b = (uint8_t)(val * 2);
                cart_bank8[0] = b; cart_bank8[1] = b + 1;
            } else if ((addr & 0xF000) == 0x7000) {
                uint8_t b = (uint8_t)(val * 2);
                cart_bank8[2] = b; cart_bank8[3] = b + 1;
            }
            break;

        case CART_MAPPER_KONAMI8:
            if ((addr & 0xF000) == 0x5000) cart_bank8[0] = val;
            else if ((addr & 0xF000) == 0x7000) cart_bank8[1] = val;
            else if ((addr & 0xF000) == 0x9000) cart_bank8[2] = val;
            else if ((addr & 0xF000) == 0xB000) cart_bank8[3] = val;
            break;

        default:
            break;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Get page pointer                                                           */
/* ────────────────────────────────────────────────────────────────────────── */
static uint8_t* get_page_ptr(int page, bool write) {
    int slot = slot_for_page(page);
    switch (slot) {
        case 0:
            if (write) return NULL;
            if (page == 0) return rom_bios;
            if (page == 1) return rom_bios + PAGE_SIZE;
            if (page == 2) return rom_basic;
            if (page == 3) return (msx_model == MSX_MODEL_2) ? rom_ext : NULL;
            return NULL;

        case 3:
            if (msx_model == MSX_MODEL_2)
                return ram_extra[ram_mapper[page] & (RAM_BANKS_MSX2 - 1)];
            return ram_main + page * PAGE_SIZE;

        default:
            return NULL;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* CPU                                                                        */
/* ────────────────────────────────────────────────────────────────────────── */
static z80 cpu;
static uint32_t cpu_cycles = 0;

/* ────────────────────────────────────────────────────────────────────────── */
/* VDP                                                                        */
/* ────────────────────────────────────────────────────────────────────────── */
static tms9918_t tms;                    // TMS9918A para MSX1 (usando tms9918.c)

static uint8_t  vram[0x20000];           // VRAM MSX2 (128KB)
static uint8_t  vdp_reg[64];
static uint8_t  vdp_status[10];
static bool     vdp_irq = false;

/* ────────────────────────────────────────────────────────────────────────── */
/* Audio AY-3-8910                                                            */
/* ────────────────────────────────────────────────────────────────────────── */
static uint8_t ay_reg2[16], ay_sel = 0;

typedef struct { 
    uint16_t period, counter; 
    uint8_t vol; 
    bool env_en; 
    double phase; 
} AYCh;

static AYCh ay_ch[3];
static uint16_t ay_np = 1, ay_nc = 0;
static uint32_t ay_lfsr = 1;
static uint8_t ay_nout = 0;
static uint16_t ay_ep = 1, ay_ec = 0;
static uint8_t ay_ev = 0;
static bool ay_hold = false, ay_alt = false, ay_att = false, ay_cont = false;

static const int16_t AY_VOL[16] = {
    0,170,240,340,480,680,960,1360,1920,2720,3840,5440,7680,10880,15360,21760
};

static void ay_write(uint8_t r, uint8_t v) {
    ay_reg2[r & 15] = v;
    for (int c = 0; c < 3; c++) {
        uint16_t p = ((uint16_t)(ay_reg2[c*2+1] & 0xF) << 8) | ay_reg2[c*2];
        ay_ch[c].period = p ? p : 1;
        ay_ch[c].vol = ay_reg2[8 + c] & 0xF;
        ay_ch[c].env_en = (ay_reg2[8 + c] & 0x10) != 0;
    }
    ay_np = ay_reg2[6] & 0x1F; if (!ay_np) ay_np = 1;
    ay_ep = ((uint16_t)ay_reg2[12] << 8) | ay_reg2[11]; if (!ay_ep) ay_ep = 1;
    if (r == 13) {
        ay_ec = 0;
        ay_att = (v & 4) != 0;
        ay_alt = (v & 2) != 0;
        ay_hold = (v & 1) != 0;
        ay_cont = (v & 8) != 0;
        ay_ev = ay_att ? 0 : 15;
    }
}

static int16_t ay_sample(void) {
    if (++ay_nc >= ay_np * 8) {
        ay_nc = 0;
        ay_lfsr ^= (ay_lfsr >> 7);
        ay_lfsr ^= (ay_lfsr << 9);
        ay_lfsr ^= (ay_lfsr >> 13);
        ay_nout = ay_lfsr & 1;
    }
    if (++ay_ec >= ay_ep * 8) {
        ay_ec = 0;
        if (!ay_hold) {
            if (ay_att) {
                if (ay_ev < 15) ay_ev++;
                else { if (!ay_cont) ay_hold = true; if (ay_alt) ay_att = !ay_att; }
            } else {
                if (ay_ev > 0) ay_ev--;
                else { if (!ay_cont) ay_hold = true; if (ay_alt) ay_att = !ay_att; }
            }
        }
    }

    int32_t mix = 0;
    for (int c = 0; c < 3; c++) {
        if (++ay_ch[c].counter >= ay_ch[c].period * 8) {
            ay_ch[c].counter = 0;
            ay_ch[c].phase = 1.0 - ay_ch[c].phase;
        }
        int tone_out = (ay_ch[c].phase >= 0.5) ? 1 : 0;
        bool te = !((ay_reg2[7] >> c) & 1);
        bool ne = !((ay_reg2[7] >> (c + 3)) & 1);

        if ((!te || tone_out) && (!ne || ay_nout))
            mix += ay_ch[c].env_en ? AY_VOL[ay_ev] : AY_VOL[ay_ch[c].vol];
    }
    mix /= 3;
    if (mix > INT16_MAX) mix = INT16_MAX;
    if (mix < INT16_MIN) mix = INT16_MIN;
    return (int16_t)mix;
}

/* Audio buffer */
static int16_t audio_buf[512];
static size_t audio_len = 0;
static double audio_next = 0.0;
static const double MSX_CPS = (double)MSX_CLOCK_HZ / (double)MSX_AUDIO_RATE;

static void audio_push(int16_t s) {
    audio_buf[audio_len++] = s;
    if (audio_len == sizeof(audio_buf)/sizeof(audio_buf[0])) {
        SDL_QueueAudio(msx_audio_dev, audio_buf, (Uint32)(audio_len * sizeof(int16_t)));
        audio_len = 0;
    }
}

static void audio_flush(void) {
    if (audio_len) {
        SDL_QueueAudio(msx_audio_dev, audio_buf, (Uint32)(audio_len * sizeof(int16_t)));
        audio_len = 0;
    }
}

static void addCycles(uint32_t d) {
    if (!d) return;
    double end = (double)cpu_cycles + d;
    while ((double)cpu_cycles < end) {
        if (audio_next <= (double)cpu_cycles) {
            audio_push(ay_sample());
            audio_next += MSX_CPS;
        }
        cpu_cycles++;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* PPI / teclado/joystick                                                     */
/* ────────────────────────────────────────────────────────────────────────── */
static uint8_t ppi_A=0xFF, ppi_B=0xFF, ppi_C=0x00, ppi_ctrl=0x82;
uint8_t msx_keymap[11];
uint8_t msx_joy[2]={0,0};

/* ────────────────────────────────────────────────────────────────────────── */
/* Memory bus callbacks                                                       */
/* ────────────────────────────────────────────────────────────────────────── */
static uint8_t mem_read_cb(void* ud, uint16_t addr) {
    (void)ud;
    addCycles(3);

    int page = addr >> 14;
    int slot = slot_for_page(page);

    if (slot == 1 && cart_size) {
        int rel = addr - 0x4000;
        if (rel >= 0 && rel < 0x8000) {
            int win8 = rel >> 13;
            int off  = rel & 0x1FFF;
            uint8_t bank = (uint8_t)(cart_bank8[win8] % cart_num_banks8());
            uint32_t a = ((uint32_t)bank << 13) | (uint32_t)off;
            if (a < cart_size) return cart_rom[a];
        }
        return 0xFF;
    }

    uint8_t* ptr = get_page_ptr(page, false);
    if (ptr) return ptr[addr & (PAGE_SIZE - 1)];

    if (msx_model == MSX_MODEL_2 && addr == 0xFFFF && slot_for_page(3) == 3)
        return (uint8_t)~subslot_reg;

    return 0xFF;
}

static void mem_write_cb(void* ud, uint16_t addr, uint8_t val) {
    (void)ud;
    addCycles(3);

    int page = addr >> 14;

    if (cart_size && slot_for_page(page) == 1) {
        cart_mapper_write(addr, val);
        return;
    }

    if (msx_model == MSX_MODEL_2 && addr == 0xFFFF && slot_for_page(3) == 3) {
        subslot_reg = val;
        return;
    }

    uint8_t* ptr = get_page_ptr(page, true);
    if (ptr) ptr[addr & (PAGE_SIZE - 1)] = val;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* I/O callbacks                                                              */
/* ────────────────────────────────────────────────────────────────────────── */
static uint8_t io_read_cb(z80* z, uint16_t port) {
    (void)z;
    addCycles(4);
    uint8_t p = port & 0xFF;

    if (msx_model == MSX_MODEL_1) {
        if (p == 0x98) return tms9918_read_data(&tms);
        if (p == 0x99) return tms9918_read_status(&tms);
    } else {
        if (p == 0x98) return 0xFF; // TODO: V9938 completo
        if (p == 0x99) return vdp_status[0];
    }

    // PPI
    if(p==0xA8) return slot_select;
    if(p==0xA9){
        uint8_t row = ppi_C & 0x0F;
        return (row<11) ? msx_keymap[row] : 0xFF;
    }
    if(p==0xAA) return ppi_C;
    if (p == 0xAA) return 0xFF; // PPI C (no implementado completamente)
    if (p == 0xA2) return ay_reg2[ay_sel & 0xF];

    return 0xFF;
}

static void io_write_cb(z80* z, uint16_t port, uint8_t val) {
    (void)z;
    addCycles(4);
    uint8_t p = port & 0xFF;

    if (msx_model == MSX_MODEL_1) {
        if (p == 0x98) { tms9918_write_data(&tms, val); return; }
        if (p == 0x99) { tms9918_write_ctrl(&tms, val); return; }
    } else {
        // MSX2 ports (parcial)
        if (p == 0x98 || p == 0x99) return; // TODO
    }

    // PPI
    if(p==0xA8){ slot_select=val; return; }
    if(p==0xAA){ ppi_C=val; return; }
    if(p==0xAB){ ppi_ctrl=val; return; }

    if (p == 0xA0) { ay_sel = val & 0xF; return; }
    if (p == 0xA1) { ay_write(ay_sel, val); return; }

    if (msx_model == MSX_MODEL_2 && p >= 0xFC && p <= 0xFF) {
        ram_mapper[p - 0xFC] = val & (RAM_BANKS_MSX2 - 1);
        return;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Render                                                                     */
/* ────────────────────────────────────────────────────────────────────────── */
void msx_render(void) {
    SDL_UpdateTexture(msx_texture, NULL, msx_pixels, MSX_W * sizeof(uint32_t));
    SDL_RenderClear(msx_renderer);
    SDL_RenderCopy(msx_renderer, msx_texture, NULL, NULL);
    SDL_RenderPresent(msx_renderer);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Frame update                                                               */
/* ────────────────────────────────────────────────────────────────────────── */
void msx_update(void) {
    uint32_t end = cpu_cycles + MSX_CYCLES_FRAME;

    while (cpu_cycles < end) {
        int cyc = z80_step(&cpu);
        cpu_cycles += (cyc <= 0) ? 4 : cyc;

        // Tick del VDP
        if (msx_model == MSX_MODEL_1) {
            tms9918_tick(&tms, cyc);
            if (tms9918_consume_nmi(&tms)) {
                z80_pulse_irq(&cpu, 0xFF);
            }
        } else {
            if (vdp_irq) {
                vdp_irq = false;
                z80_pulse_irq(&cpu, 0xFF);
            }
        }
    }

    // VBlank flag
    if (msx_model == MSX_MODEL_1) {
        tms.status |= 0x80;
    }

    audio_flush();
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Reset                                                                      */
/* ────────────────────────────────────────────────────────────────────────── */
void msx_reset(void) {
    z80_init(&cpu);
    cpu.read_byte = mem_read_cb;
    cpu.write_byte = mem_write_cb;
    cpu.port_in = io_read_cb;
    cpu.port_out = io_write_cb;

    cpu_cycles = 0;
    audio_next = 0.0;
    audio_len = 0;

    // Reset VDP
    if (msx_model == MSX_MODEL_1) {
        tms9918_reset(&tms);
        tms.fb   = msx_pixels;
        tms.fb_w = MSX_W;
    } else {
        memset(vdp_reg, 0, sizeof(vdp_reg));
        memset(vdp_status, 0, sizeof(vdp_status));
        memset(vram, 0, sizeof(vram));
        vdp_irq = false;
    }

    // Reset cartucho y slots
    subslot_reg = 0;
    cart_reset_banks();

    memset(ay_reg2, 0, sizeof(ay_reg2));
    ay_sel = 0;
    for (int i = 0; i < 3; i++) {
        ay_ch[i].period = 1;
        ay_ch[i].counter = 0;
        ay_ch[i].vol = 0;
        ay_ch[i].env_en = false;
        ay_ch[i].phase = 0.0;
    }

    memset(msx_keymap, 0xFF, sizeof(msx_keymap));
    memset(msx_joy, 0, sizeof(msx_joy));

    // Display ON por defecto
    if (msx_model == MSX_MODEL_2)
        vdp_reg[1] |= 0x40;

    printf("MSX%d: RESET (slot_select=%02X cart=%u)\n",
           (int)msx_model, slot_select, (unsigned)cart_size);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Init                                                                       */
/* ────────────────────────────────────────────────────────────────────────── */
int msx_init(const char* rom_dir, MSXModel model) {
    msx_model = model;

    // Inicializar TMS9918
    tms9918_init(&tms, msx_pixels, MSX_W);

    // Estado inicial del cartucho
    cart_size = 0;
    cart_mapper = CART_MAPPER_NONE;
    cart_reset_banks();
    memset(cart_rom, 0xFF, sizeof(cart_rom));

    // Slot por defecto: BIOS en slot 0, RAM en slot 3
    slot_select = (0 << 0) | (0 << 2) | (0 << 4) | (3 << 6);

    // Cargar ROMs (mantengo tu lógica original)
    char path[512];

    if (model == MSX_MODEL_1) {
        snprintf(path, sizeof(path), "%s/msx1.rom", rom_dir);
        uint8_t big[0xC000];
        if (try_load(path, big, 0xC000) == 0) {  // función try_load definida abajo
            memcpy(rom_bios,  big,        0x8000);
            memcpy(rom_basic, big + 0x8000, 0x4000);
        } else {
            // fallback a archivos separados
            snprintf(path, sizeof(path), "%s/msx1_bios.rom", rom_dir);
            if (try_load(path, rom_bios, 0x8000) < 0) {
                snprintf(path, sizeof(path), "%s/cbios_main_msx1.rom", rom_dir);
                if (try_load(path, rom_bios, 0x8000) < 0) {
                    fprintf(stderr, "MSX1: BIOS no encontrada\n");
                    return -1;
                }
            }
            snprintf(path, sizeof(path), "%s/msx1_basic.rom", rom_dir);
            if (try_load(path, rom_basic, 0x4000) < 0) {
                snprintf(path, sizeof(path), "%s/cbios_basic.rom", rom_dir);
                if (try_load(path, rom_basic, 0x4000) < 0) {
                    fprintf(stderr, "MSX1: BASIC no encontrada\n");
                    return -1;
                }
            }
        }
    } else {
        // MSX2 (lógica original simplificada)
        snprintf(path, sizeof(path), "%s/msx2.rom", rom_dir);
        uint8_t big[0x10000];
        if (try_load(path, big, 0x10000) == 0) {
            memcpy(rom_bios,  big,        0x8000);
            memcpy(rom_ext,   big + 0x8000, 0x4000);
            memcpy(rom_basic, big + 0xC000, 0x4000);
        } else {
            fprintf(stderr, "MSX2: BIOS no encontrada (usa msx2.rom o archivos separados)\n");
            return -1;
        }
    }

    memset(ram_main, 0, sizeof(ram_main));
    memset(ram_extra, 0, sizeof(ram_extra));

    msx_reset();

    printf("MSX%d: INIT OK (usando TMS9918 externo) rom_dir=%s\n", (int)model, rom_dir);
    return 0;
}

void msx_quit(void) {
    // nada especial por ahora
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Loader helper (necesaria para msx_init)                                    */
/* ────────────────────────────────────────────────────────────────────────── */
static int try_load(const char* path, uint8_t* dst, size_t sz) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(dst, 1, sz, f);
    fclose(f);
    return (rd == sz) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Load cart (sin cambios importantes)                                        */
/* ────────────────────────────────────────────────────────────────────────── */
bool msx_load_rom(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "No se pudo abrir cartucho: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > CART_MAX_SIZE) {
        fclose(f);
        return false;
    }

    memset(cart_rom, 0xFF, sizeof(cart_rom));
    cart_size = (uint32_t)fread(cart_rom, 1, size, f);
    fclose(f);

    cart_mapper = detect_mapper_by_size(cart_size);
    cart_reset_banks();

    // Configurar slot 1 para el cartucho
    slot_select = (0 << 0) | (1 << 2) | (1 << 4) | (3 << 6);  // Page0=0, Page1=1, Page2=1, Page3=3

    printf("Cartucho cargado: %s (%u KB) Mapper:%d SlotSelect:%02X\n",
           path, cart_size / 1024, (int)cart_mapper, slot_select);

    msx_reset();
    return true;
}

bool msx_load_cas(const char* path) {
    printf("MSX CAS: no implementado (%s)\n", path);
    return false;
}
