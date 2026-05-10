#ifndef TEHKANWC_H
#define TEHKANWC_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "z80/jgz80/z80.h"

// ---------------------------------------------------------------------------
// Tehkan World Cup Hardware Specification
// ---------------------------------------------------------------------------
// CPU: Triple Z80 architecture
// - Main CPU: 3.072 MHz
// - Sub CPU (Graphics): 3.072 MHz
// - Sound CPU: 3.072 MHz
// Memory: Shared RAM (2KB) + Individual RAM per CPU
// Resolution: 256x224 (scrollable)
// ---------------------------------------------------------------------------

// Timing
#define TEHKANWC_CPU_CLOCK_HZ    3072000
#define TEHKANWC_FRAME_RATE      60
#define TEHKANWC_TSTATES_PER_FRAME  (TEHKANWC_CPU_CLOCK_HZ / TEHKANWC_FRAME_RATE)

// Screen geometry
#define TEHKANWC_SCREEN_WIDTH    256
#define TEHKANWC_SCREEN_HEIGHT   224
#define TEHKANWC_DISPLAY_WIDTH   256
#define TEHKANWC_DISPLAY_HEIGHT  224

// Memory map (per CPU)
#define TEHKANWC_ROM_SIZE        0x4000   // 16KB ROM per CPU
#define TEHKANWC_RAM_SIZE        0x2000   // 8KB RAM per CPU
#define TEHKANWC_SHARED_RAM_SIZE 0x0800   // 2KB Shared RAM

// CPU indices
#define TEHKANWC_CPU_MAIN        0
#define TEHKANWC_CPU_SUB         1
#define TEHKANWC_CPU_SOUND       2
#define TEHKANWC_CPU_COUNT       3

// Video RAM configuration
#define TEHKANWC_VIDEORAM_SIZE   0x2000  // 8KB
#define TEHKANWC_VIDEORAM_BANKS  2       // Double buffer support

// ---------------------------------------------------------------------------
// Trackball Input Structure
// ---------------------------------------------------------------------------
typedef struct {
    int x_current;      // Current X position
    int y_current;      // Current Y position
    int x_delta;        // Delta X (change)
    int y_delta;        // Delta Y (change)
    uint8_t buttons;    // Button state
} TrackballInput;

// ---------------------------------------------------------------------------
// Scrolling Control Structure
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t scroll_x[2];  // Scroll X (2 registers)
    uint8_t scroll_y;     // Scroll Y
    int     offset_x;     // Computed X offset
    int     offset_y;     // Computed Y offset
} ScrollState;

// ---------------------------------------------------------------------------
// LED Display (Gridiron Fight variant)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t led0;        // Player 0 LED
    uint8_t led1;        // Player 1 LED
} LEDState;

// ---------------------------------------------------------------------------
// Video Rendering State
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t* videoram;           // Primary VRAM
    uint8_t* tmpbitmap_data;     // Temporary bitmap (2x width)
    uint8_t* dirtybuffer;        // Dirty flag buffer
    ScrollState scroll;
    LEDState led;
    
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t      framebuffer[TEHKANWC_DISPLAY_WIDTH * TEHKANWC_DISPLAY_HEIGHT];
} VideoState;

// ---------------------------------------------------------------------------
// CPU Memory Bank Structure
// ---------------------------------------------------------------------------
typedef struct {
    z80 cpu;
    uint8_t* rom;
    uint8_t* ram;
    uint8_t* mem_map[4];    // 4x 16KB banks
} CPUBank;

// ---------------------------------------------------------------------------
// Main Emulator Structure
// ---------------------------------------------------------------------------
typedef struct {
    // Triple Z80 CPUs
    CPUBank cpus[TEHKANWC_CPU_COUNT];
    
    // Shared memory
    uint8_t shared_ram[TEHKANWC_SHARED_RAM_SIZE];
    
    // Video subsystem
    VideoState video;
    
    // Input (Trackball per player)
    TrackballInput trackball[2];
    uint8_t buttons;
    
    // CPU synchronization
    int cycle_counter[TEHKANWC_CPU_COUNT];
    int frames_run;
    
    // Control flags
    bool quit;
    bool turbo_mode;
    
} TehkanWC;

// ---------------------------------------------------------------------------
// API - Initialization & Control
// ---------------------------------------------------------------------------
void tehkanwc_init(TehkanWC* hw);
void tehkanwc_destroy(TehkanWC* hw);

int  tehkanwc_load_rom_main(TehkanWC* hw, const char* filename);
int  tehkanwc_load_rom_sub(TehkanWC* hw, const char* filename);
int  tehkanwc_load_rom_sound(TehkanWC* hw, const char* filename);

void tehkanwc_run_frame(TehkanWC* hw);
void tehkanwc_render(TehkanWC* hw);

// Input
void tehkanwc_handle_trackball(TehkanWC* hw, int player, int dx, int dy);
void tehkanwc_handle_buttons(TehkanWC* hw, uint8_t buttons);
void tehkanwc_handle_key(TehkanWC* hw, SDL_Scancode key, bool pressed);

// CPU control
void tehkanwc_set_sub_cpu_halt(TehkanWC* hw, bool halted);
void tehkanwc_sound_command(TehkanWC* hw, uint8_t command);

// ---------------------------------------------------------------------------
// Internal Helpers (video)
// ---------------------------------------------------------------------------
void tehkanwc_video_init(TehkanWC* hw);
void tehkanwc_video_update(TehkanWC* hw);
void tehkanwc_video_render(TehkanWC* hw);

#endif // TEHKANWC_H
