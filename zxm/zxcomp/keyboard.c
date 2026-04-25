/*
 * keyboard.c - CPC keyboard matrix emulation
 *
 * Maps SDL2 keycodes to CPC keyboard matrix positions.
 * Matrix is active-low: 0=key pressed, 1=released.
 */

#include "keyboard.h"
#include <string.h>
#include <stdio.h>

void keyboard_init(keyboard_t *kb)
{
    /* All keys released = 0xFF per row */
    memset(kb->matrix, 0xFF, sizeof(kb->matrix));
}

/*
 * CPC key positions: {row, bit}
 * bit 0 = LSB of the row byte
 */
typedef struct {
    SDL_Keycode sdl;
    int         row;
    int         bit;
} key_map_t;

static const key_map_t key_map[] = {
    /* Row 0 */
    { SDLK_UP,          0, 0 },
    { SDLK_RIGHT,       0, 1 },
    { SDLK_DOWN,        0, 2 },
    { SDLK_F9,          0, 3 },
    { SDLK_F6,          0, 4 },
    { SDLK_F3,          0, 5 },
    { SDLK_KP_ENTER,    0, 6 },
    { SDLK_KP_PERIOD,   0, 7 },

    /* Row 1 */
    { SDLK_LEFT,        1, 0 },
    { SDLK_F10,         1, 1 },  /* COPY -> F10 (F10 quits via main, but still maps) */
    { SDLK_F7,          1, 2 },
    { SDLK_F8,          1, 3 },
    { SDLK_F5,          1, 4 },
    { SDLK_F1,          1, 5 },
    { SDLK_F2,          1, 6 },
    //{ SDLK_F0,          1, 7 },  /* F0 = not standard SDL, skip */

    /* Row 2 */
    { SDLK_HOME,        2, 0 },  /* CLR SCR */
    { SDLK_LEFTBRACKET, 2, 1 },
    { SDLK_RETURN,      2, 2 },
    { SDLK_RIGHTBRACKET,2, 3 },
    { SDLK_BACKSPACE,   2, 4 },
    { SDLK_MINUS,       2, 5 },
    { SDLK_6,           2, 6 },  /* ^ -> shift+6 handled */
    { SDLK_BACKQUOTE,   2, 7 },  /* @ */

    /* Row 3 */
    { SDLK_p,           3, 0 },
    { SDLK_SEMICOLON,   3, 1 },
    { SDLK_COMMA,       3, 2 },
    { SDLK_PERIOD,      3, 3 },
    { SDLK_SLASH,       3, 4 },
    { SDLK_QUOTE,       3, 5 },
    { SDLK_BACKSLASH,   3, 6 },
    { SDLK_CAPSLOCK,    3, 7 },

    /* Row 4 */
    { SDLK_i,           4, 0 },
    { SDLK_o,           4, 1 },
    { SDLK_PAGEUP,      4, 2 },  /* [ on CPC numpad */
    { SDLK_l,           4, 3 },
    { SDLK_COLON,       4, 4 },  /* : */
    { SDLK_k,           4, 5 },
    { SDLK_m,           4, 6 },
    { SDLK_n,           4, 7 },

    /* Row 5 */
    { SDLK_u,           5, 0 },
    { SDLK_y,           5, 1 },
    { SDLK_SPACE,       5, 2 },
    { SDLK_h,           5, 3 },
    { SDLK_j,           5, 4 },
    { SDLK_b,           5, 5 },
    { SDLK_g,           5, 6 },
    { SDLK_v,           5, 7 },

    /* Row 6 */
    { SDLK_r,           6, 0 },
    { SDLK_t,           6, 1 },
    { SDLK_w,           6, 2 },
    { SDLK_e,           6, 3 },
    { SDLK_q,           6, 4 },
    { SDLK_f,           6, 5 },
    { SDLK_d,           6, 6 },
    { SDLK_c,           6, 7 },

    /* Row 7 */
    { SDLK_0,           7, 0 },
    { SDLK_9,           7, 1 },
    { SDLK_8,           7, 2 },
    { SDLK_7,           7, 3 },
    /* 6 already in row 2 for ^, map digit 6 here too */
    { SDLK_5,           7, 5 },
    { SDLK_4,           7, 6 },
    { SDLK_3,           7, 7 },

    /* Row 8 */
    { SDLK_2,           8, 0 },
    { SDLK_1,           8, 1 },
    { SDLK_ESCAPE,      8, 2 },
    { SDLK_TAB,         8, 3 },
    { SDLK_LCTRL,       8, 4 },
    { SDLK_RCTRL,       8, 4 },
    { SDLK_LSHIFT,      8, 5 },
    { SDLK_RSHIFT,      8, 6 },
    { SDLK_a,           8, 7 },

    /* Row 9 */
    { SDLK_LALT,        9, 6 },
    { SDLK_RALT,        9, 6 },
    { SDLK_DELETE,      9, 7 },
    { SDLK_s,           9, 0 },  /* extra mappings */
    { SDLK_x,           9, 1 },
    { SDLK_z,           9, 2 },

    /* Numpad */
    { SDLK_KP_0,        7, 0 },
    { SDLK_KP_1,        8, 1 },
    { SDLK_KP_2,        0, 2 },
    { SDLK_KP_3,        0, 5 },
    { SDLK_KP_4,        1, 0 },
    { SDLK_KP_5,        1, 4 },
    { SDLK_KP_6,        0, 1 },
    { SDLK_KP_7,        0, 3 },
    { SDLK_KP_8,        0, 0 },
    { SDLK_KP_9,        0, 3 },
    { SDLK_KP_PLUS,     1, 2 },
    { SDLK_KP_MINUS,    2, 5 },

    { 0, 0, 0 } /* sentinel */
};

static void set_key(keyboard_t *kb, SDL_Keycode sym, int pressed)
{
    for (int i = 0; key_map[i].sdl != 0; i++) {
        if (key_map[i].sdl == sym) {
            int row = key_map[i].row;
            int bit = key_map[i].bit;
            if (pressed) {
                kb->matrix[row] &= ~(1 << bit);  /* active low: clear bit */
            } else {
                kb->matrix[row] |=  (1 << bit);  /* release: set bit */
            }
            return;
        }
    }
}

void keyboard_key_down(keyboard_t *kb, SDL_Keycode sym, SDL_Keymod mod)
{
    (void)mod;
    set_key(kb, sym, 1);
}

void keyboard_key_up(keyboard_t *kb, SDL_Keycode sym, SDL_Keymod mod)
{
    (void)mod;
    set_key(kb, sym, 0);
}

uint8_t keyboard_read_row(keyboard_t *kb, uint8_t row)
{
    if (row < KB_ROWS)
        return kb->matrix[row];
    return 0xFF;
}
