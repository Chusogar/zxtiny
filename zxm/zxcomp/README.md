# CPC6128 Emulator

Emulador de Amstrad CPC6128 escrito en C con SDL2.

## Características

- **CPU Z80** completa (todos los opcodes documentados + no documentados, DDCB/FDCB, ED prefix)
- **Gate Array** con 3 modos de vídeo (0/1/2) y paleta hardware de 27 colores
- **CRTC 6845** para timing de vídeo
- **PPI 8255** (teclado, PSG control, cassette)
- **PSG AY-3-8912** con 3 canales de tono, ruido, generador de envolvente y salida estéreo
- **FDC uPD765** con soporte DSK estándar y DSK extendido (CPCEMU/Extended)
- **Teclado** mapeado completo SDL2 → matriz CPC
- **Snapshots SNA** v1/v2/v3 (carga y guardado)
- **Drag & drop** de ficheros `.sna` y `.dsk`
- **Modo warp** (velocidad máxima)
- **128 KB RAM** con banking completo (8 configuraciones)

---

## Estructura del proyecto

```
cpc6128/
├── main.c          — Punto de entrada, SDL2, bucle principal
├── cpc.c / .h      — Sistema CPC, bus I/O, coordinación
├── z80.c / .h      — Core Z80 (compatible Carmikel)
├── memory.c / .h   — Mapa de memoria, banking 128K, ROMs
├── gate_array.c/.h — Gate Array: vídeo, paleta, modos
├── crtc.c / .h     — MC6845 CRTC
├── ppi.c / .h      — Intel 8255 PPI
├── psg.c / .h      — AY-3-8912 PSG (audio)
├── fdc.c / .h      — uPD765 FDC + parser DSK
├── keyboard.c / .h — Matriz de teclado CPC
├── sna.c / .h      — Snapshots SNA
└── Makefile
```

---

## Compilación

### Requisitos

- GCC (o Clang)
- SDL2 (`libsdl2-dev` en Debian/Ubuntu)

```bash
# Ubuntu/Debian
sudo apt install build-essential libsdl2-dev

# Fedora
sudo dnf install gcc SDL2-devel

# macOS (Homebrew)
brew install sdl2
```

### Compilar

```bash
make          # Release optimizado
make debug    # Con AddressSanitizer y UBSan
make clean    # Limpiar objetos
```

---

## ROMs necesarias

Coloca los ficheros ROM en el directorio `roms/`:

```
roms/
├── cpc6128.rom   (OS ROM,    16384 bytes)
├── basic.rom     (BASIC 1.1, 16384 bytes)
└── amsdos.rom    (AMSDOS,    16384 bytes)
```

> **Nota:** Amstrad plc ha dado permiso explícito para distribuir
> libremente las ROMs de CPC para propósitos de emulación.
> Puedes encontrarlas en [https://www.cpcwiki.eu/index.php/CPC_ROMs](https://www.cpcwiki.eu/index.php/CPC_ROMs)

---

## Uso

```bash
# Arrancar con BASIC
./cpc6128

# Cargar snapshot SNA
./cpc6128 -sna mi_juego.sna

# Insertar disco DSK
./cpc6128 -dsk mi_disco.dsk

# Snapshot + disco
./cpc6128 -sna estado.sna -dsk juego.dsk

# ROM alternativa
./cpc6128 -rom roms/mi_os.rom -dsk juego.dsk
```

También puedes **arrastrar y soltar** ficheros `.sna` o `.dsk`
directamente sobre la ventana del emulador.

---

## Controles de teclado

| Tecla       | Función                    |
|-------------|----------------------------|
| **F1**      | Reset del CPC              |
| **F10**     | Salir del emulador         |
| **F12**     | Activar/desactivar warp    |
| Drag & Drop | Cargar `.sna` o `.dsk`     |

El teclado del PC se mapea directamente al teclado CPC.
Las teclas especiales del CPC:

| CPC          | PC            |
|--------------|---------------|
| COPY         | F10           |
| CLR          | Home          |
| DEL          | Backspace     |
| ENTER        | Return        |
| Cursor keys  | Arrow keys    |
| Fxx          | F1–F9         |

---

## Formato DSK

Se soportan ambas variantes:
- **Standard DSK** (`MV - CPCEMU Disk-File`) — discos de tamaño fijo
- **Extended DSK** (`EXTENDED CPC DSK File`) — sectores de tamaño variable

Compatible con los dumps de disco generados por CPCemu, WinAPE, JOYCE, etc.

---

## Formato SNA

Soporte completo de snapshots SNA versiones 1, 2 y 3:
- Registros Z80 completos (incluyendo alternos, IX, IY, I, R)
- Estado Gate Array (paleta, modo de vídeo)
- Registros CRTC
- Estado PPI y PSG
- Volcado de 64 KB o 128 KB de RAM

Para guardar el estado actual, implementa la tecla en `main.c`:
```c
} else if (e.key.keysym.sym == SDLK_F5) {
    sna_save(cpc, "snapshot.sna");
}
```

---

## Sobre el core Z80

El fichero `z80.c` implementa el Z80 completo con la misma interfaz
que el core de **Carmikel** (https://github.com/carmikel/z80):

- Callbacks `mem_read`, `mem_write`, `io_read`, `io_write`
- Campo `userdata` para pasar contexto
- `z80_step()` ejecuta una instrucción y devuelve los T-states
- `z80_run()` ejecuta hasta consumir N T-states
- Soporte NMI y INT (modos 0, 1 y 2)
- Instrucciones no documentadas (SLL, IX/IY half-regs, DDCB/FDCB)

Para usar el core original de Carmikel en lugar de esta implementación,
simplemente reemplaza `z80.c` manteniendo `z80.h` como está
(la interfaz es compatible).

---

## Licencia

Este emulador se distribuye bajo la licencia MIT.
Las ROMs de Amstrad no están incluidas y tienen sus propios términos
(distribución libre para emulación según Amstrad plc).
