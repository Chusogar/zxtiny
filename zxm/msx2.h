#ifndef MSX2_H
#define MSX2_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Temporización MSX2 PAL (V9938)
//   CPU clock:  3 579 545 Hz
//   VDP lines:  313 (PAL)
//   CPU clocks per line: 228  (VDP 1368 dots / 6)
//   Frame T-states: 313 × 228 = 71 364
//   Frame rate:  ≈ 50.16 Hz
// ---------------------------------------------------------------------------
#define MSX_CPU_CLOCK       3579545
#define MSX_LINES_PER_FRAME 313
#define MSX_CLOCKS_PER_LINE 228
#define MSX_TSTATES_FRAME   (MSX_LINES_PER_FRAME * MSX_CLOCKS_PER_LINE) // 71364
#define MSX_AUDIO_RATE      44100
#define MSX_SAMPLES_FRAME   (MSX_AUDIO_RATE / 50)  // 882

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------
#define MSX_SCREEN_W   256
#define MSX_SCREEN_H   212
#define MSX_BORDER_H   16
#define MSX_BORDER_V   14
#define MSX_FULL_W     (MSX_SCREEN_W + 2 * MSX_BORDER_H)  // 288
#define MSX_FULL_H     (MSX_SCREEN_H + 2 * MSX_BORDER_V)  // 240
#define MSX_SCALE      2

// ---------------------------------------------------------------------------
// V9938 VDP
// ---------------------------------------------------------------------------
#define VDP_VRAM_SIZE    (128 * 1024)
#define VDP_NUM_REGS     48        // R0..R23, R32..R46 + padding
#define VDP_NUM_STATUS   10        // S0..S9

typedef struct {
    uint8_t  vram[VDP_VRAM_SIZE];
    uint8_t  regs[VDP_NUM_REGS];
    uint8_t  status[VDP_NUM_STATUS];
    uint32_t palette[16];          // ARGB8888 expanded palette
    uint8_t  pal_rgb[16][3];       // 3-bit R,G,B per entry

    // Registro de acceso
    uint8_t  latch;                // primer byte del par escritura puerto 1
    bool     latch_flag;           // true = esperando segundo byte
    uint8_t  read_buf;             // buffer de prefetch para lectura VRAM
    uint32_t vram_addr;            // dirección VRAM (17 bits)
    bool     vram_write;           // true = modo escritura

    // Paleta
    uint8_t  pal_latch;
    bool     pal_flag;             // primer/segundo byte paleta

    // Registro indirecto (puerto 3 / 0x9B)
    // (R17 ya está en regs[])

    // Estado de renderizado
    int      scanline;             // línea actual (0..312)
    int      frame_counter;

    // Interrupciones
    bool     irq_vblank;           // VBLANK pendiente
    bool     irq_hblank;           // HBLANK pendiente (línea R19)

    // Comandos VDP
    int      cmd_sx, cmd_sy;
    int      cmd_dx, cmd_dy;
    int      cmd_nx, cmd_ny;
    int      cmd_clr;
    int      cmd_arg;
    int      cmd_op;               // comando actual (0=idle)
    int      cmd_phase;
    int      cmd_px, cmd_py;       // contadores internos
    bool     cmd_busy;
    uint8_t  cmd_byte;             // byte leído (LMCM/POINT)
} MSX2_VDP;

// ---------------------------------------------------------------------------
// AY-3-8910 PSG
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  regs[16];
    uint8_t  addr;                 // registro seleccionado

    // Generadores de tono
    uint16_t tone_period[3];
    uint16_t tone_cnt[3];
    uint8_t  tone_out[3];

    // Generador de ruido
    uint8_t  noise_period;
    uint8_t  noise_cnt;
    uint32_t noise_shift;
    uint8_t  noise_out;

    // Envolvente
    uint16_t env_period;
    uint16_t env_cnt;
    uint8_t  env_step;
    uint8_t  env_shape;
    bool     env_holding;
    bool     env_alternate;
    bool     env_attack;

    // Acumulador de ciclos
    int      cycle_accum;
} MSX2_PSG;

// ---------------------------------------------------------------------------
// PPI 8255
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  slot_sel;             // Port A: primary slot selection
    uint8_t  port_c;               // Port C: keyboard col + cassette + beeper
    uint8_t  key_row;              // bits 3-0 de port_c: fila teclado
} MSX2_PPI;

// ---------------------------------------------------------------------------
// Slot system y memoria
// ---------------------------------------------------------------------------
#define MSX_RAM_PAGES    16        // 256 KB RAM (16 × 16 KB)
#define MSX_ROM_CART_MAX (1024*1024)

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_ROM,
    SLOT_RAM,
    SLOT_CART
} MSX2SlotType;

typedef struct {
    uint8_t* data;                 // puntero a los datos
    uint32_t size;                 // tamaño real
    bool     writable;
} MSX2SlotPage;

// MegaROM mapper types
typedef enum {
    MAPPER_NONE = 0,
    MAPPER_ASCII8,
    MAPPER_ASCII16,
    MAPPER_KONAMI,
    MAPPER_KONAMI_SCC
} MSX2MapperType;

typedef struct {
    uint8_t  ram[MSX_RAM_PAGES * 16384];  // 256 KB RAM
    uint8_t  bios[32768];                  // Main BIOS ROM (32 KB)
    uint8_t  subrom[16384];                // Sub-ROM / ExtBIOS (16 KB)
    uint8_t  diskrom[16384];               // Disk ROM (16 KB)
    bool     has_diskrom;

    uint8_t  cart_data[MSX_ROM_CART_MAX];   // Cartridge ROM data
    uint32_t cart_size;
    MSX2MapperType cart_mapper;
    uint8_t  cart_bank[4];                 // Bank selection for MegaROMs (4 banks)

    // Slot configuration
    uint8_t  primary_sel;                  // Port 0xA8 (PPI port A)
    uint8_t  secondary_sel[4];             // Sub-slot registers per primary slot
    bool     expanded[4];                  // Is primary slot expanded?

    // Memory mapper (RAM)
    uint8_t  mapper_reg[4];                // Ports 0xFC-0xFF: RAM page per CPU page

    // Read/write page cache (recalculated on slot change)
    uint8_t* rd[4];                        // Read pointers for 4 CPU pages
    uint8_t* wr[4];                        // Write pointers (NULL if ROM)
} MSX2_MEM;

// ---------------------------------------------------------------------------
// CAS tape
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t pos;                          // current read position
    bool     loaded;
} MSX2_CAS;

// ---------------------------------------------------------------------------
// DSK disk
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t* data;
    uint32_t size;
    int      sides;
    int      tracks;
    int      sectors;                      // per track
    bool     loaded;
} MSX2_DSK;

// ---------------------------------------------------------------------------
// RTC RP5C01
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  reg_addr;                     // selected register (0-15)
    uint8_t  mode;                         // register 13: block select (bits 1-0)
    uint8_t  ram[4][13];                   // 4 blocks × 13 nibbles
} MSX2_RTC;

// ---------------------------------------------------------------------------
// Estructura principal MSX2
// ---------------------------------------------------------------------------
typedef struct {
    z80       cpu;
    MSX2_MEM  mem;
    MSX2_VDP  vdp;
    MSX2_PSG  psg;
    MSX2_PPI  ppi;
    MSX2_RTC  rtc;
    MSX2_CAS  cas;
    MSX2_DSK  dsk;

    // Teclado MSX: 11 filas × 8 columnas
    uint8_t   keyboard[11];

    // Joystick (leído a través de PSG register 14)
    uint8_t   joy1;                        // bits: 5=trgB 4=trgA 3=right 2=left 1=down 0=up (active low)

    // SDL
    SDL_Window*       window;
    SDL_Renderer*     renderer;
    SDL_Texture*      texture;
    uint32_t          framebuffer[MSX_FULL_W * MSX_FULL_H];
    SDL_AudioDeviceID audio_dev;
    float             audio_buffer[MSX_SAMPLES_FRAME];
    int               audio_pos;

    bool   quit;
    bool   turbo_mode;
} MSX2;

// Prototipos públicos
void msx2_init(MSX2* m);
void msx2_destroy(MSX2* m);
int  msx2_load_bios(MSX2* m, const char* dir);
int  msx2_load_rom(MSX2* m, const char* filename);
int  msx2_load_cas(MSX2* m, const char* filename);
int  msx2_load_dsk(MSX2* m, const char* filename);
void msx2_run_frame(MSX2* m);
void msx2_render(MSX2* m);
void msx2_handle_key(MSX2* m, SDL_Scancode key, bool pressed);

#endif // MSX2_H
