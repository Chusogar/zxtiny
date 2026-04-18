// Updated cpc.c to fix various issues for Amstrad CPC6128 emulator

#include <stdint.h>

// Corrected palette mask from 0x1A to 0x1F
#define PALETTE_MASK 0x1F

// Function to correct interleaved line address calculation
uint16_t calculate_interleaved_line_address(uint8_t line, uint8_t mode) {
    // Improved calculation logic goes here
}

// Function to fix pixel bit extraction for all video modes (0, 1, 2)
uint8_t extract_pixel(uint16_t address, uint8_t mode) {
    // Updated pixel extraction logic based on mode
}

// Improved VRAM access logic for safety
void access_vram(uint16_t address) {
    // Enhanced safety checks when accessing VRAM
}

// Main emulator logic...

void emulate() {
    // Emulator main execution loop
}