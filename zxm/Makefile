# ============================================================
#  zxm emulators - Makefile
#  Requires: gcc, SDL2 (libsdl2-dev)
#
#  Build:   make          (builds all)
#           make zx       (ZX Spectrum only)
#           make frogger  (Frogger arcade only)
#  Clean:   make clean
# ============================================================

CC      = gcc
Z80_SRC = ../src/cpu/Z80/jgz80/z80.c
Z80_INC = -I.

# Create symlink for z80 headers if not present
Z80_LINK = z80

# SDL2 flags
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo "-I/usr/include/SDL2 -D_REENTRANT")
SDL2_LIBS   := $(shell sdl2-config --libs   2>/dev/null || echo "-lSDL2")

CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter $(SDL2_CFLAGS)
LDFLAGS = $(SDL2_LIBS) -lm

all: $(Z80_LINK) zx frogger

$(Z80_LINK):
	@test -L $(Z80_LINK) || ln -sf ../src/cpu/Z80 $(Z80_LINK)

zx: $(Z80_LINK) zx.c zx.h tzx.c tzx.h
	$(CC) $(CFLAGS) $(Z80_INC) zx.c tzx.c $(Z80_SRC) $(LDFLAGS) -o $@
	@echo "Built: zx"

frogger: $(Z80_LINK) frogger.c frogger.h
	$(CC) $(CFLAGS) $(Z80_INC) frogger.c $(Z80_SRC) $(LDFLAGS) -o $@
	@echo "Built: frogger"

clean:
	rm -f zx frogger
	rm -f $(Z80_LINK)

.PHONY: all clean
