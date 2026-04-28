# ============================================================
#  Emuladores ZXM - Makefile
#  Requiere: gcc, SDL2 (libsdl2-dev)
# ============================================================

CC      = gcc
Z80_SRC = z80/jgz80/z80.c

SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo "-I/usr/include/SDL2 -D_REENTRANT")
SDL2_LIBS   := $(shell sdl2-config --libs   2>/dev/null || echo "-lSDL2")

CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic \
          -Wno-unused-parameter -Wno-unused-variable \
          $(SDL2_CFLAGS)
LDFLAGS = $(SDL2_LIBS) -lm

# ---- ZX Spectrum ----
zx: zx.c tzx.c $(Z80_SRC) zx.h tzx.h
	$(CC) $(CFLAGS) -o $@ zx.c tzx.c $(Z80_SRC) $(LDFLAGS)

# ---- MSX2 ----
msx2: msx2.c $(Z80_SRC) msx2.h
	$(CC) $(CFLAGS) -o $@ msx2.c $(Z80_SRC) $(LDFLAGS)

# ---- CPC ----
cpc: cpc.c cpc_fdc.c $(Z80_SRC) cpc.h cpc_fdc.h
	$(CC) $(CFLAGS) -o $@ cpc.c cpc_fdc.c $(Z80_SRC) $(LDFLAGS)

all: zx msx2 cpc

clean:
	rm -f zx msx2 cpc

.PHONY: all clean
