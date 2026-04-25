#pragma once
/*
 * cpc_fdc.h  –  Emulador del FDC NEC µPD765A para Amstrad CPC 6128
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  Hardware real                                                       │
 * │                                                                      │
 * │  Chip    : NEC µPD765A (compatible con Intel 8272A)                  │
 * │  Unidades: 2 unidades (A: y B:), doble cara, doble densidad (DSDD)  │
 * │  Puertos I/O del CPC:                                                │
 * │    0xFB7E  – Motor on/off  (escritura)                               │
 * │    0xFB7F  – FDC status register  (lectura)                          │
 * │    0xFB7F  – FDC data register    (lectura/escritura)                │
 * │                                                                      │
 * │  Formato DSK soportado:                                              │
 * │    · DSK estándar de Amstrad ("MV - CPC" / "EXTENDED CPC DSK")      │
 * │    · Hasta 42 pistas, 2 caras, 9-18 sectores por pista              │
 * │    · Sectores de tamaño variable (128B a 8192B)                      │
 * │    · Gaps, sector IDs arbitrarios, sector size codes                 │
 * │                                                                      │
 * │  Comandos µPD765 implementados:                                      │
 * │    0x02  Read Track (leer pista completa)                            │
 * │    0x03  Specify (parámetros de timing)                              │
 * │    0x04  Sense Drive Status                                          │
 * │    0x05  Write Data                                                  │
 * │    0x06  Read Data                                                   │
 * │    0x07  Recalibrate (ir a pista 0)                                  │
 * │    0x08  Sense Interrupt Status                                      │
 * │    0x09  Write Deleted Data                                          │
 * │    0x0A  Read ID                                                     │
 * │    0x0C  Read Deleted Data                                           │
 * │    0x0D  Format Track                                                │
 * │    0x0F  Seek                                                        │
 * │    0x11  Scan Equal                                                  │
 * └──────────────────────────────────────────────────────────────────────┘
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────────────────────
// Límites físicos del sistema de disco
// ─────────────────────────────────────────────────────────────────────────────
#define FDC_MAX_DRIVES      2     // A: y B:
#define FDC_MAX_TRACKS     84     // 42 pistas × 2 caras
#define FDC_MAX_SECTORS    29     // máximo sectores/pista en DSK extendido
#define FDC_MAX_SECTOR_SZ 8192   // tamaño máximo de sector (size code 6)

// ─────────────────────────────────────────────────────────────────────────────
// Estructura de un sector en memoria
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  track;         // C: cilindro (pista)
    uint8_t  side;          // H: cabeza (cara)
    uint8_t  sector_id;     // R: identificador de sector
    uint8_t  size_code;     // N: 0=128B 1=256B 2=512B 3=1024B ...
    uint8_t  st1;           // ST1: estado guardado para result phase
    uint8_t  st2;           // ST2: estado guardado para result phase
    uint16_t data_len;      // longitud real de los datos
    uint8_t* data;          // puntero a los datos del sector
} FDC_Sector;

// ─────────────────────────────────────────────────────────────────────────────
// Estructura de una pista (track)
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    int        num_sectors;
    FDC_Sector sectors[FDC_MAX_SECTORS];
} FDC_Track;

// ─────────────────────────────────────────────────────────────────────────────
// Imagen de disco montada en una unidad
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    bool      inserted;           // hay disco montado
    bool      write_protected;    // protección contra escritura
    bool      extended;           // formato extendido (EXTENDED CPC DSK)
    int       num_tracks;         // pistas por cara
    int       num_sides;          // número de caras (1 o 2)
    FDC_Track tracks[FDC_MAX_TRACKS]; // [track + side * num_tracks]
    uint8_t*  raw_data;           // buffer con los datos crudos del DSK
    size_t    raw_size;           // tamaño del buffer
} FDC_Disk;

// ─────────────────────────────────────────────────────────────────────────────
// Estado de una unidad de disco
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    FDC_Disk disk;
    int      current_track;    // cabeza posicionada en esta pista física
    bool     motor_on;         // motor encendido
    bool     ready;            // unidad lista
} FDC_Drive;

// ─────────────────────────────────────────────────────────────────────────────
// Estados internos del FDC (máquina de estados)
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    FDC_IDLE,           // esperando comando
    FDC_COMMAND,        // recibiendo bytes de comando
    FDC_EXECUTION,      // ejecutando (transferencia de datos)
    FDC_RESULT          // fase de resultado (CPU leyendo status)
} FDC_Phase;

// ─────────────────────────────────────────────────────────────────────────────
// Registro de estado principal (MSR – Main Status Register)
// ─────────────────────────────────────────────────────────────────────────────
#define FDC_MSR_DB0   0x01   // Drive 0 busy (seek en curso)
#define FDC_MSR_DB1   0x02   // Drive 1 busy
#define FDC_MSR_CB    0x10   // FDC busy (command en ejecución)
#define FDC_MSR_EXM   0x20   // Execution mode (DMA/no-DMA)
#define FDC_MSR_DIO   0x40   // Data direction: 1=FDC→CPU, 0=CPU→FDC
#define FDC_MSR_RQM   0x80   // Request for Master: FDC listo para byte

// ─────────────────────────────────────────────────────────────────────────────
// ST0 – Status Register 0
// ─────────────────────────────────────────────────────────────────────────────
#define FDC_ST0_US0   0x01   // Unit Select bit 0
#define FDC_ST0_US1   0x02   // Unit Select bit 1
#define FDC_ST0_HD    0x04   // Head Address
#define FDC_ST0_NR    0x08   // Not Ready
#define FDC_ST0_EC    0x10   // Equipment Check
#define FDC_ST0_SE    0x20   // Seek End
#define FDC_ST0_IC_OK 0x00   // Interrupt Code: Normal termination
#define FDC_ST0_IC_AT 0x40   // Interrupt Code: Abnormal termination
#define FDC_ST0_IC_IC 0x80   // Interrupt Code: Invalid command
#define FDC_ST0_IC_RP 0xC0   // Interrupt Code: Ready changed (polling)

// ─────────────────────────────────────────────────────────────────────────────
// ST1 – Status Register 1
// ─────────────────────────────────────────────────────────────────────────────
#define FDC_ST1_MA    0x01   // Missing Address Mark
#define FDC_ST1_NW    0x02   // Not Writable (write protected)
#define FDC_ST1_ND    0x04   // No Data (sector ID no encontrado)
#define FDC_ST1_OR    0x10   // Over Run
#define FDC_ST1_DE    0x20   // Data Error (CRC error)
#define FDC_ST1_EN    0x80   // End of Cylinder

// ─────────────────────────────────────────────────────────────────────────────
// ST2 – Status Register 2
// ─────────────────────────────────────────────────────────────────────────────
#define FDC_ST2_MD    0x01   // Missing Address Mark in Data Field
#define FDC_ST2_BC    0x02   // Bad Cylinder
#define FDC_ST2_SN    0x04   // Scan Not Satisfied
#define FDC_ST2_SH    0x08   // Scan Equal Hit
#define FDC_ST2_WC    0x10   // Wrong Cylinder
#define FDC_ST2_DD    0x20   // Data Error in Data Field (CRC)
#define FDC_ST2_CM    0x40   // Control Mark (sector borrado)

// ─────────────────────────────────────────────────────────────────────────────
// Estructura principal del FDC
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    // Unidades físicas
    FDC_Drive drives[FDC_MAX_DRIVES];

    // Máquina de estados
    FDC_Phase phase;
    uint8_t   cmd_buf[9];    // bytes de comando (máx. 9)
    int       cmd_len;       // bytes esperados para el comando actual
    int       cmd_pos;       // bytes recibidos hasta ahora
    uint8_t   res_buf[7];    // bytes de resultado (máx. 7)
    int       res_len;       // bytes de resultado disponibles
    int       res_pos;       // bytes de resultado ya leídos

    // Parámetros de la operación en curso
    uint8_t   sel_drive;     // unidad seleccionada (0/1)
    uint8_t   sel_side;      // cara seleccionada (0/1)
    int       cur_sector;    // índice del sector actual en la pista
    int       data_pos;      // posición dentro del sector en transferencia
    bool      irq_pending;   // interrupción pendiente para la CPU

    // Registros de estado (últimas operaciones)
    uint8_t   st0, st1, st2;

    // Parámetros Specify (SRT, HUT, HLT, ND)
    uint8_t   srt;           // Step Rate Time
    uint8_t   hut;           // Head Unload Time
    uint8_t   hlt;           // Head Load Time
    bool      nd;            // Non-DMA mode (siempre true en CPC)

    // Control de motor (puerto 0xFA7E del CPC)
    bool      motor;         // motor encendido

} FDC;

// ─────────────────────────────────────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────────────────────────────────────

/* Inicializa el FDC a estado de reset. */
void fdc_init(FDC* fdc);

/* Reset hardware del FDC. */
void fdc_reset(FDC* fdc);

/* Lectura del puerto de status (0xFB7F con A0=0). */
uint8_t fdc_read_status(FDC* fdc);

/* Lectura del puerto de datos (0xFB7F con A0=1). */
uint8_t fdc_read_data(FDC* fdc);

/* Escritura del puerto de datos (0xFB7F con A0=1). */
void fdc_write_data(FDC* fdc, uint8_t val);

/* Control de motor: val bit0=drive A motor, bit1=drive B motor. */
void fdc_motor_control(FDC* fdc, uint8_t val);

/* Carga un archivo .DSK en la unidad especificada (0=A:, 1=B:).
   Retorna true si la imagen se cargó correctamente. */
bool fdc_load_dsk(FDC* fdc, int drive, const char* path);

/* Expulsa el disco de la unidad especificada. */
void fdc_eject(FDC* fdc, int drive);

/* Retorna true si hay una interrupción pendiente del FDC. */
bool fdc_irq(FDC* fdc);

/* Debe llamarse en cada ciclo de CPU para actualizar el estado interno.
   delta: ciclos de CPU transcurridos desde la última llamada. */
void fdc_tick(FDC* fdc, int delta);
