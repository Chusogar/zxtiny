# ============================================================
#  zxm emulators - Makefile
#  Requires: gcc, SDL2 (libsdl2-dev)
#
#  Build:   make          (builds all)
#           make zx       (ZX Spectrum only)
#           make frogger  (Frogger arcade only)
#           make c64      (Commodore 64 only)
#  Clean:   make clean
# ============================================================

CC      = gcc
Z80_SRC = ../src/cpu/Z80/jgz80/z80.c
M6502_SRC = ../src/cpu/m6502/m6502.c

# Symlinks for CPU headers
Z80_LINK   = z80
M6502_LINK = m6502

# SDL2 flags
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo "-I/usr/include/SDL2 -D_REENTRANT")
SDL2_LIBS   := $(shell sdl2-config --libs   2>/dev/null || echo "-lSDL2")

CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter $(SDL2_CFLAGS)
LDFLAGS = $(SDL2_LIBS) -lm

all: $(Z80_LINK) $(M6502_LINK) zx frogger c64

$(Z80_LINK):
	@test -L $(Z80_LINK) || ln -sf ../src/cpu/Z80 $(Z80_LINK)

$(M6502_LINK):
	@test -L $(M6502_LINK) || ln -sf ../src/cpu/m6502 $(M6502_LINK)

zx: $(Z80_LINK) zx.c zx.h tzx.c tzx.h
	$(CC) $(CFLAGS) zx.c tzx.c $(Z80_SRC) $(LDFLAGS) -o $@
	@echo "Built: zx"

frogger: $(Z80_LINK) frogger.c frogger.h
	$(CC) $(CFLAGS) frogger.c $(Z80_SRC) $(LDFLAGS) -o $@
	@echo "Built: frogger"

c64: $(M6502_LINK) c64.c c64.h
	$(CC) $(CFLAGS) c64.c $(M6502_SRC) $(LDFLAGS) -o $@
	@echo "Built: c64"

clean:
	rm -f zx frogger c64
	rm -f $(Z80_LINK) $(M6502_LINK)

.PHONY: all clean
