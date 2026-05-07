#pragma once
/*
 * beta128.h – Emulador Beta 128 Disk Interface (WD1793 FDC)
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  Hardware real                                                       │
 * │                                                                      │
 * │  Interfaz : Beta 128 (Technology Research)                           │
 * │  FDC chip : Western Digital WD1793 (compatible WD1770)               │
 * │  Unidades : Hasta 4 unidades, doble cara, 80 pistas                 │
 * │  Sistema  : TR-DOS 5.xx                                              │
 * │                                                                      │
 * │  Puertos I/O (cuando la ROM Beta está paginada):                     │
 * │    0x1F  – Command / Status Register                                 │
 * │    0x3F  – Track Register                                            │
 * │    0x5F  – Sector Register                                           │
 * │    0x7F  – Data Register                                             │
 * │    0xFF  – System Register (drive select, side, etc.)                │
 * │                                                                      │
 * │  Formatos de imagen soportados:                                      │
 * │    · TRD (imagen cruda de disco TR-DOS)                              │
 * │    · SCL (contenedor compacto de ficheros TR-DOS)                    │
 * │                                                                      │
 * │  Geometría TR-DOS estándar:                                          │
 * │    · 80 pistas, 2 caras, 16 sectores/pista, 256 bytes/sector        │
 * │    · Total: 655360 bytes (640 KB)                                    │
 * └──────────────────────────────────────────────────────────────────────┘
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constantes del disco TR-DOS
// ─────────────────────────────────────────────────────────────────────────────
#define BETA_MAX_DRIVES     4
#define BETA_TRACKS_PER_SIDE 80
#define BETA_SECTORS_PER_TRACK 16
#define BETA_SECTOR_SIZE    256
#define BETA_TRACK_SIZE     (BETA_SECTORS_PER_TRACK * BETA_SECTOR_SIZE) // 4096
#define BETA_DISK_SIZE      (BETA_TRACKS_PER_SIDE * 2 * BETA_TRACK_SIZE) // 655360

// ─────────────────────────────────────────────────────────────────────────────
// WD1793 Status Register bits
// ─────────────────────────────────────────────────────────────────────────────

// Type I commands (RESTORE, SEEK, STEP)
#define WD_ST_BUSY        0x01
#define WD_ST_INDEX       0x02  // index pulse
#define WD_ST_TRACK0      0x04  // track 0
#define WD_ST_CRCERR      0x08  // CRC error
#define WD_ST_SEEKERR     0x10  // seek error
#define WD_ST_HEADLOADED  0x20  // head loaded
#define WD_ST_WRPROTECT   0x40  // write protected
#define WD_ST_NOTREADY    0x80  // not ready

// Type II/III commands (READ/WRITE SECTOR, READ ADDRESS, etc.)
#define WD_ST_DRQ         0x02  // data request
#define WD_ST_LOSTDATA    0x04  // lost data
#define WD_ST_RNFERR      0x10  // record not found
#define WD_ST_RECTYPE     0x20  // record type (deleted mark)
// 0x40 = write fault (write) or 0 (read)
// 0x80 = not ready

// ─────────────────────────────────────────────────────────────────────────────
// WD1793 Command types
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    WD_CMD_NONE = 0,
    WD_CMD_RESTORE,       // Type I
    WD_CMD_SEEK,          // Type I
    WD_CMD_STEP,          // Type I
    WD_CMD_STEP_IN,       // Type I
    WD_CMD_STEP_OUT,      // Type I
    WD_CMD_READ_SEC,      // Type II
    WD_CMD_WRITE_SEC,     // Type II
    WD_CMD_READ_ADDR,     // Type III
    WD_CMD_READ_TRACK,    // Type III
    WD_CMD_WRITE_TRACK,   // Type III
    WD_CMD_FORCE_INT      // Type IV
} WD_CmdType;

// ─────────────────────────────────────────────────────────────────────────────
// Disco montado en una unidad Beta
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    bool     inserted;
    bool     write_protected;
    int      num_tracks;    // pistas por cara (normalmente 80)
    int      num_sides;     // 1 o 2
    uint8_t* data;          // buffer completo del disco (max 655360 bytes)
    size_t   data_size;     // tamaño real del buffer
} BetaDisk;

// ─────────────────────────────────────────────────────────────────────────────
// Estado de una unidad Beta
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    BetaDisk disk;
    int      current_track;
    bool     motor_on;
} BetaDrive;

// ─────────────────────────────────────────────────────────────────────────────
// Estado del WD1793 + Beta 128 interface
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    BetaDrive drives[BETA_MAX_DRIVES];

    // Registros WD1793
    uint8_t   status;       // Status Register (lectura puerto 0x1F)
    uint8_t   track_reg;    // Track Register (puerto 0x3F)
    uint8_t   sector_reg;   // Sector Register (puerto 0x5F)
    uint8_t   data_reg;     // Data Register (puerto 0x7F)
    uint8_t   command_reg;  // último comando escrito

    // System Register (puerto 0xFF del Beta 128)
    uint8_t   system_reg;   // bits: D0-D1=drive, D2=reset(?), D3=head,
                            //       D4=HLT, D6=MFM, D7=intrq

    // Estado interno
    WD_CmdType  cmd_type;
    bool        busy;
    bool        drq;        // Data Request
    bool        intrq;      // Interrupt Request

    // Transferencia de datos
    int         data_pos;
    int         data_len;
    uint8_t*    data_ptr;   // puntero al sector actual en el disco
    bool        data_write; // true si estamos escribiendo
    bool        multi_sector; // flag 'm' del comando

    // Dirección de step (para STEP sin dirección)
    int         step_dir;   // +1 o -1

    // Drive/side seleccionados
    uint8_t     sel_drive;
    uint8_t     sel_side;

    // Paginación Beta ROM
    bool        beta_active; // ROM Beta paginada en 0x0000-0x3FFF

    // Contador de index pulse
    int         index_counter;

} Beta128;

// ─────────────────────────────────────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────────────────────────────────────

void beta128_init(Beta128* beta);
void beta128_reset(Beta128* beta);

// Lectura de puertos (0x1F, 0x3F, 0x5F, 0x7F, 0xFF)
uint8_t beta128_read(Beta128* beta, uint16_t port);

// Escritura de puertos (0x1F, 0x3F, 0x5F, 0x7F, 0xFF)
void beta128_write(Beta128* beta, uint16_t port, uint8_t val);

// Carga un archivo .TRD en la unidad especificada (0..3)
bool beta128_load_trd(Beta128* beta, int drive, const char* path);

// Carga un archivo .SCL en la unidad especificada (0..3)
bool beta128_load_scl(Beta128* beta, int drive, const char* path);

// Expulsa el disco de la unidad
void beta128_eject(Beta128* beta, int drive);

// Retorna true si hay un INTRQ pendiente
bool beta128_intrq(Beta128* beta);

// Llamar cada ciertos T-states para simular index pulse, etc.
void beta128_tick(Beta128* beta, int delta);


