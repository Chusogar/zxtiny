---
name: testing-zxtiny
description: Build, test, and run the zxtiny ZX Spectrum emulator. Use when verifying emulation changes for any model (48K, 128K, +3, Pentagon, Scorpion).
---

## Build

```bash
cd zxm
gcc -o zx zx.c beta128.c cpc_fdc.c tzx.c z80/jgz80/z80.c -I. -lSDL2 -lm
```

Pre-existing warnings in `cpc_fdc.c` (unused variables) are expected and harmless.

## ROMs

ROMs must be in the working directory (`zxm/`). Expected ROM files per model:

| Model | ROMs needed |
|-------|------------|
| 48K | `zx48.rom` (16KB) |
| 128K | `zx128.rom` (32KB) |
| +3 | `plus3-0.rom` through `plus3-3.rom` (16KB each) |
| Pentagon 128/1024 | `128tr.rom`, `zx128_1.rom`, `trdos.rom` (16KB each) |
| Scorpion ZS 256 | `scorp0.rom` through `scorp3.rom` (16KB each) |

If Pentagon ROMs are missing, you can create substitutes:
- `dd if=zx128.rom of=128tr.rom bs=16384 count=1` (ROM 0)
- `dd if=zx128.rom of=zx128_1.rom bs=16384 skip=1 count=1` (ROM 1)
- `cp scorp3.rom trdos.rom` (TR-DOS ROM from Scorpion)

Note: These substitutes use the standard 128K ROM, not the Pentagon-specific one. The boot menu will lack "128 TR-DOS" option. Real Pentagon ROMs are needed for full TR-DOS testing.

## Running

```bash
cd zxm
./zx --48k              # ZX Spectrum 48K
./zx --128k             # ZX Spectrum 128K
./zx --plus3 file.dsk   # +3 with disk
./zx --pentagon file.trd  # Pentagon 128 with TR-DOS disk
./zx --pentagon1024     # Pentagon 1024 (1024KB RAM)
./zx --scorpion         # Scorpion ZS 256
```

Disk files (.trd, .scl) are passed as positional arguments after the model flag.

## Unit Testing

Paging logic can be tested with standalone C programs that include `zx.h` and replicate the paging functions from `zx.c`. Example:

```bash
gcc -o test_pent1024 test_pent1024.c -I. -lSDL2 -lm
./test_pent1024
```

Key pattern: allocate a `ZXSpectrum` struct, set model/port values, call the paging function, then verify `mem_map[]` pointers match expected RAM banks.

## SDL2 GUI Testing Limitations

- SDL2 windows in headless/remote environments may not respond to `xdotool` keyboard events. The computer tool's keyboard input may also not work with SDL2.
- Visual verification (window title, boot menu rendering, screen output) works via screenshots.
- For keyboard interaction testing (menu navigation, TR-DOS commands), the user needs to test on their own machine.
- ALSA errors ("cannot find card '0'") are expected in headless environments and don't affect emulation.

## Key Architecture Notes

- Memory map: `mem_map[0-3]` = 4 × 16KB slots ($0000-$3FFF, $4000-$7FFF, $8000-$BFFF, $C000-$FFFF)
- Pentagon 1024 paging: port $EFF7 (bit2=mode, bit3=RAM overlay) + extended $7FFD decode
- Beta 128 auto-paging: activates TR-DOS ROM when PC in $3D00-$3DFF with ROM 1 paged
- WD1793: Type I commands (RESTORE/SEEK/STEP) have different status format than Type II/III
- SCL/TRD disk info sector: bytes $8E1-$8F3 must be correctly populated

## No CI

This repo has no CI pipeline. Verify manually with build + unit tests + visual inspection.
