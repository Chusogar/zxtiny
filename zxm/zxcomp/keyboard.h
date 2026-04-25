/*
 * keyboard.h - Amstrad CPC keyboard matrix
 *
 * The CPC keyboard is an 10x8 matrix.
 * PPI Port B bits 0-7 = key states for selected row
 * PPI Port C bits 0-3 = row select (0-9)
 *
 * Matrix map (row, bit):
 *   Row 0: CURSOR_UP, CURSOR_RIGHT, CURSOR_DOWN, F9, F6, F3, ENTER, FDOT
 *   Row 1: CURSOR_LEFT, COPY, F7, F8, F5, F1, F2, F0
 *   Row 2: CLRSCR, OPEN_SQ, RETURN, CLOSE_SQ, DELETE, MINUS, HAT, @
 *   Row 3: P, SEMICOLON, COMMA, PERIOD, FSLASH, APOSTROPHE, BSLASH, CAPS
 *   Row 4: I, O, PGUP, L, COLON, K, M, N
 *   Row 5: U, Y, SPACE, H, J, B, G, V
 *   Row 6: R, T, W, E, Q, F, D, C
 *   Row 7: 0, 9, 8, 7, 6, 5, 4, 3
 *   Row 8: 2, 1, ESC, TAB, CTRL, SHIFT_L, SHIFT_R, A
 *   Row 9: NOPE, NOPE, NOPE, NOPE, NOPE, NOPE, ALT, DEL
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <SDL2/SDL.h>

#define KB_ROWS 10
#define KB_COLS  8

typedef struct keyboard_s {
    uint8_t matrix[KB_ROWS];  /* 0=pressed, 1=released (active low) */
} keyboard_t;

void    keyboard_init    (keyboard_t *kb);
void    keyboard_key_down(keyboard_t *kb, SDL_Keycode sym, SDL_Keymod mod);
void    keyboard_key_up  (keyboard_t *kb, SDL_Keycode sym, SDL_Keymod mod);
uint8_t keyboard_read_row(keyboard_t *kb, uint8_t row);

#endif /* KEYBOARD_H */
