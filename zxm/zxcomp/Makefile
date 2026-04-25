# ============================================================
#  CPC6128 Emulator - Makefile
#  Requires: gcc, SDL2 (libsdl2-dev)
#
#  Build:   make
#  Run:     ./cpc6128
#  Install: make install
# ============================================================

CC      = gcc
TARGET  = cpc6128
SRCS    = main.c cpc.c z80.c memory.c gate_array.c crtc.c \
          ppi.c psg.c fdc.c keyboard.c sna.c
OBJS    = $(SRCS:.c=.o)

# SDL2 flags (use sdl2-config if available)
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo "-I/usr/include/SDL2 -D_REENTRANT")
SDL2_LIBS   := $(shell sdl2-config --libs   2>/dev/null || echo "-lSDL2")

CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic \
          -Wno-unused-parameter \
          $(SDL2_CFLAGS) \
          -MMD -MP

LDFLAGS = $(SDL2_LIBS) -lm

# Debug build
debug: CFLAGS += -g3 -DDEBUG -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

# Release (default)
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Include auto-generated dependency files
-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	@mkdir -p /usr/local/share/$(TARGET)/roms
	@echo "Install ROMs to /usr/local/share/$(TARGET)/roms/"

roms:
	@mkdir -p roms
	@echo "Place the following ROM files in ./roms/:"
	@echo "  cpc6128.rom  (CPC6128 OS ROM, 16384 bytes)"
	@echo "  basic.rom    (Locomotive BASIC 1.1, 16384 bytes)"
	@echo "  amsdos.rom   (AMSDOS, 16384 bytes)"
	@echo ""
	@echo "These ROMs are copyrighted by Amstrad plc."
	@echo "Amstrad has given permission for free distribution"
	@echo "of their CPC ROMs for emulation purposes."

help:
	@echo "CPC6128 Emulator"
	@echo ""
	@echo "Usage: ./$(TARGET) [options]"
	@echo "  -sna <file>   Load SNA snapshot"
	@echo "  -dsk <file>   Insert DSK disk image (drive A:)"
	@echo "  -rom <file>   Override OS ROM (default: roms/cpc6128.rom)"
	@echo ""
	@echo "Runtime keys:"
	@echo "  F1            Hard reset"
	@echo "  F10           Quit"
	@echo "  F12           Toggle warp speed"
	@echo "  Drag & drop   Load .sna or .dsk files"
	@echo ""
	@echo "Build targets:"
	@echo "  make          Release build"
	@echo "  make debug    Debug build (ASan + UBSan)"
	@echo "  make clean    Remove build artifacts"
	@echo "  make roms     Show ROM setup instructions"

.PHONY: all debug clean install roms help
