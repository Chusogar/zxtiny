
CFLAGS=-g -Wall -DDEBUG -DLSB_FIRST -I. -I/usr/include/SDL -D_GNU_SOURCE=1 -D_REENTRANT
LDFLAGS=-g -L/usr/lib/x86_64-linux-gnu -lSDL2
BAS2BIN=./bas2bin
BIN2GTP=./bin2gtp

all: ZXSpectrum ZXSpectrum.c cpu/Z80/Z80.c cpu/Z80/Z80.h

ZXSpectrum.o : ZXSpectrum.c
	$(CC) -c ZXSpectrum.c -o ZXSpectrum.o $(CFLAGS)

Z80.o : cpu/Z80/Z80.c cpu/Z80/Z80.h
	$(CC) -c Z80.c -o Z80.o $(CFLAGS)
	
ZXSpectrum: ZXSpectrum.o Z80/Z80.o
	$(CC) ZXSpectrum.o Z80/Z80.o -o ZXSpectrum $(LDFLAGS)

clean:
	rm -f ZXSpectrum *.o cpu/Z80/*.o cpu/Z80/dasm
