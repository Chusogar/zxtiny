/*
 * coleco.c  –  ColecoVision emulator core
 *
 * · Z80A @ 3.58 MHz
 * · TMS9918A VDP (separado a tms9918.c/.h)
 * · SN76489   – 3 canales de tono + 1 ruido
 * · I/O 0xBE/0xBF: VDP
 * · I/O 0x80/0xC0: mandos (modo joystick / keypad simplificado)
 * · I/O 0xFF: PSG (SN76489) write
 *
 * NOTAS:
 *  - ColecoVision: VDP INT -> Z80 NMI (no IRQ maskable).
 *  - Evitar doble conteo de ciclos: NO sumar ciclos dentro de mem/io callbacks.
 */

#include "coleco.h"
#include "tms9918.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#ifdef _WIN32
#  define strcasecmp _stricmp
#endif

extern SDL_Renderer*     cv_renderer;
extern SDL_Texture*      cv_texture;
extern SDL_AudioDeviceID cv_audio_dev;

uint32_t cv_pixels[COLECO_H * COLECO_W];

// ── Memoria ───────────────────────────────────────────────────────────────────
static uint8_t  bios[0x2000];      // 8 KB BIOS
static uint8_t  cart[0x8000];      // hasta 32 KB cartucho (mapeado en 0x8000-0xFFFF)
static uint8_t  ram [0x0400];      // 1 KB RAM en 0x6000-0x7FFF (espejo)
static uint32_t cart_size = 0;

// ── CPU ───────────────────────────────────────────────────────────────────────
static z80      cpu;
static uint32_t cpu_cycles = 0;

// ── Mandos ────────────────────────────────────────────────────────────────────
uint8_t cv_joy[2]    = {0,0};
uint8_t cv_keypad[2] = {0xFF,0xFF};
static uint8_t joy_mode = 0;       // 0=joystick mode, 1=keypad mode

// ── VDP (TMS9918A) ───────────────────────────────────────────────────────────
static tms9918_t vdp;

// ── SN76489 ───────────────────────────────────────────────────────────────────
typedef struct {
    uint16_t period;
    uint16_t counter;
    uint8_t  vol;       // 0-15
    uint8_t  out;       // 0 o 1
} SNChan;

static SNChan   sn_ch[4];
static uint8_t  sn_latch_reg = 0;
static uint16_t sn_noise_lfsr = 0x8000;
static uint8_t  sn_noise_out = 0;

static const int16_t SN_VOL[16] = {
    32767, 26028, 20675, 16422, 13045, 10362, 8231, 6538,
    5193, 4125, 3276, 2602, 2067, 1642, 1304, 0
};

static void sn_write(uint8_t val) {
    if (val & 0x80) {
        sn_latch_reg = (val >> 4) & 7;
        uint8_t data = val & 0x0F;
        uint8_t ch = sn_latch_reg >> 1;

        if (!(sn_latch_reg & 1)) {
            sn_ch[ch].period = (sn_ch[ch].period & 0x3F0) | data;
            if (!sn_ch[ch].period) sn_ch[ch].period = 1;
        } else {
            sn_ch[ch].vol = data;
        }
    } else {
        uint8_t ch = sn_latch_reg >> 1;
        if (!(sn_latch_reg & 1)) {
            sn_ch[ch].period = (sn_ch[ch].period & 0x00F) | ((uint16_t)(val & 0x3F) << 4);
            if (!sn_ch[ch].period) sn_ch[ch].period = 1;
        } else {
            sn_ch[ch].vol = val & 0x0F;
        }
    }
}

static int16_t sn_sample(void) {
    int32_t out = 0;

    for (int c = 0; c < 3; c++) {
        if (++sn_ch[c].counter >= sn_ch[c].period) {
            sn_ch[c].counter = 0;
            sn_ch[c].out ^= 1;
        }
        out += sn_ch[c].out ? SN_VOL[sn_ch[c].vol] : -SN_VOL[sn_ch[c].vol];
    }

    // Ruido (canal 3)
    if (++sn_ch[3].counter >= sn_ch[3].period) {
        sn_ch[3].counter = 0;

        int feedback = (sn_noise_lfsr & 1);
        sn_noise_lfsr = (sn_noise_lfsr >> 1) |
                        ((feedback ^ ((sn_noise_lfsr >> 1) & 1)) << 15);
        sn_noise_out = (uint8_t)feedback;
    }
    out += sn_noise_out ? SN_VOL[sn_ch[3].vol] : -SN_VOL[sn_ch[3].vol];

    out /= 4;
    if (out > INT16_MAX) out = INT16_MAX;
    if (out < INT16_MIN) out = INT16_MIN;
    return (int16_t)out;
}

// ── Audio (rápido, sin loops finos) ───────────────────────────────────────────
static int16_t audio_buf[512];
static size_t  audio_buf_len = 0;
static double  audio_accum = 0.0;

static const double CV_CYC_PER_SAMPLE =
    (double)COLECO_CLOCK_HZ / (double)COLECO_AUDIO_RATE;

static void audio_push(int16_t s){
    audio_buf[audio_buf_len++] = s;
    if(audio_buf_len == (sizeof(audio_buf)/sizeof(audio_buf[0]))){
        SDL_QueueAudio(cv_audio_dev, audio_buf, (Uint32)(audio_buf_len * sizeof(int16_t)));
        audio_buf_len = 0;
    }
}

static void audio_flush(void){
    if(audio_buf_len){
        SDL_QueueAudio(cv_audio_dev, audio_buf, (Uint32)(audio_buf_len * sizeof(int16_t)));
        audio_buf_len = 0;
    }
}

static void audio_tick(int cyc){
    audio_accum += (double)cyc;
    while(audio_accum >= CV_CYC_PER_SAMPLE){
        audio_push(sn_sample());
        audio_accum -= CV_CYC_PER_SAMPLE;
    }
}

// ── Bus de memoria ────────────────────────────────────────────────────────────
static uint8_t mem_read_cb(void* ud, uint16_t addr) {
    (void)ud;

    if (addr < 0x2000) return bios[addr];
    if (addr >= 0x6000 && addr < 0x8000) return ram[addr & 0x03FF];
    if (addr >= 0x8000 && cart_size > 0) return cart[addr - 0x8000];

    return 0xFF;
}

static void mem_write_cb(void* ud, uint16_t addr, uint8_t val) {
    (void)ud;

    if (addr >= 0x6000 && addr < 0x8000)
        ram[addr & 0x03FF] = val;
}

// ── Bus de I/O ────────────────────────────────────────────────────────────────
static uint8_t io_read_cb(z80* z, uint16_t port) {
    (void)z;
    uint8_t p = port & 0xFF;

    // VDP
    if (p == 0xBE) return tms9918_read_data(&vdp);
    if (p == 0xBF) return tms9918_read_status(&vdp);

	
    if (p == 0xFC || p == 0xFF) {
        if (joy_mode == 0) return ~cv_joy[0] & 0x7F;
        else               return cv_keypad[0];
    }
    if (p == 0xFD || p == 0xFE) {
        if (joy_mode == 0) return ~cv_joy[1] & 0x7F;
        else               return cv_keypad[1];
    }
    return 0xFF;
}

static void io_write_cb(z80* z, uint16_t port, uint8_t val) {
    (void)z;
    uint8_t p = port & 0xFF;

    // VDP
    if (p == 0xBE) { tms9918_write_data(&vdp, val); return; }
    if (p == 0xBF) { tms9918_write_ctrl(&vdp, val); return; }

    // SN76489: típico en Coleco (write-only)
    if (p == 0xFF) { sn_write(val); return; }

    // Control mandos (muy simplificado): selecciona modo joystick/keypad
    if (p == 0x80 || p == 0xC0) {
        joy_mode = (val & 0x01) ? 1 : 0;
        return;
    }
}

// ── Render SDL ────────────────────────────────────────────────────────────────
void cv_render(void) {
    SDL_UpdateTexture(cv_texture, NULL, cv_pixels, COLECO_W * (int)sizeof(uint32_t));
    SDL_RenderClear(cv_renderer);
    SDL_RenderCopy(cv_renderer, cv_texture, NULL, NULL);
    SDL_RenderPresent(cv_renderer);
}

// ── Frame update ──────────────────────────────────────────────────────────────
void cv_update(void) {
    const uint32_t frame_end = cpu_cycles + COLECO_CYCLES_FRAME;

    while (cpu_cycles < frame_end) {
        int cyc = z80_step(&cpu);
        if (cyc <= 0) cyc = 1;

        cpu_cycles += (uint32_t)cyc;

        // VDP + Audio por ciclos
        tms9918_tick(&vdp, cyc);
        audio_tick(cyc);

        // Coleco: VDP INT -> NMI
        if (tms9918_consume_nmi(&vdp)) {
            z80_pulse_nmi(&cpu);
        }
    }

    audio_flush();
}

// ── Reset ─────────────────────────────────────────────────────────────────────
void cv_reset(void) {
    z80_init(&cpu);

    cpu.userdata   = NULL;
    cpu.read_byte  = mem_read_cb;
    cpu.write_byte = mem_write_cb;
    cpu.port_in    = io_read_cb;
    cpu.port_out   = io_write_cb;

    cpu_cycles = 0;

    // Reset PSG
    memset(sn_ch, 0, sizeof(sn_ch));
    sn_latch_reg = 0;
    sn_noise_lfsr = 0x8000;
    sn_noise_out = 0;

    // VDP
    tms9918_init(&vdp, cv_pixels, COLECO_W);
    tms9918_reset(&vdp);
}

// ── Init/Quit ────────────────────────────────────────────────────────────────
static int load_file(const char* p, uint8_t* buf, size_t sz){
    FILE* f = fopen(p, "rb");
    if(!f) return -1;
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    return (rd > 0) ? 0 : -1;
}

int cv_init(const char* rom_dir) {
    char path[512];

    snprintf(path, sizeof(path), "%s/coleco.rom", rom_dir);
    if (load_file(path, bios, 0x2000) < 0) {
        snprintf(path, sizeof(path), "%s/bios.rom", rom_dir);
        if (load_file(path, bios, 0x2000) < 0) {
            fprintf(stderr, "ColecoVision: coleco.rom no encontrada en %s\n", rom_dir);
            return -1;
        }
    }

    cv_reset();
    return 0;
}

void cv_quit(void) {
    // No gestionamos SDL aquí (lo hará coleco_main normalmente)
    // Solo limpiamos estado si se desea.
    cart_size = 0;
}

// ── Carga ROM (incluye .col robusto) ──────────────────────────────────────────
bool cv_load_rom(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) { fclose(f); return false; }

    uint8_t* tmp = (uint8_t*)malloc((size_t)file_size);
    if (!tmp) { fclose(f); return false; }

    size_t rd = fread(tmp, 1, (size_t)file_size, f);
    fclose(f);

    if (rd == 0) { free(tmp); return false; }

    const char* ext = strrchr(path, '.');
    bool is_col = ext && (strcasecmp(ext, ".col") == 0);

    memset(cart, 0, sizeof(cart));
    cart_size = 0;

    if (is_col) {
        // .col: a veces incluye cabecera; cogemos los últimos 32KB si sobra
        if (rd > 0x8000) {
            memcpy(cart, tmp + (rd - 0x8000), 0x8000);
            cart_size = 0x8000;
        } else {
            memcpy(cart, tmp, rd);
            cart_size = (uint32_t)rd;
        }
    } else {
        // raw .rom: si empieza por AA55, saltamos 2 bytes
        size_t off = 0;
        if (rd >= 2 && tmp[0] == 0xAA && tmp[1] == 0x55)
            off = 2;

        size_t copy = rd - off;
        if (copy > 0x8000) copy = 0x8000;

        memcpy(cart, tmp + off, copy);
        cart_size = (uint32_t)copy;
    }

    free(tmp);

    cv_reset();
    printf("ColecoVision: cartucho %s (%u bytes, %s, file=%ld)\n",
           path, cart_size, is_col ? ".col" : "raw", file_size);

    return cart_size > 0;
}