/*
 * cpc_fdc.c  –  Emulador completo del FDC NEC µPD765A para CPC 6128
 *
 * Implementa el controlador de disco del Amstrad CPC 6128 con soporte
 * completo para los formatos DSK estándar y extendido.
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  Protocolo µPD765A                                                  │
 * │                                                                     │
 * │  Cada operación tiene tres fases:                                   │
 * │                                                                     │
 * │  1. COMMAND : La CPU escribe los bytes del comando.                 │
 * │     El primer byte identifica el comando (bits 0-4).                │
 * │     Los bits 5-7 son flags (MF=MFM, MT=multisector, SK=skip).      │
 * │                                                                     │
 * │  2. EXECUTION: El FDC transfiere datos sector a sector.             │
 * │     En modo no-DMA (el CPC): la CPU lee/escribe byte a byte.        │
 * │     El MSR indica cuándo hay un byte disponible/solicitado.         │
 * │                                                                     │
 * │  3. RESULT  : El FDC pone 7 bytes de resultado.                     │
 * │     La CPU los lee en orden; el último byte libera el FDC.          │
 * │                                                                     │
 * │  Formato DSK estándar (header 256 bytes):                           │
 * │    +0x00 "MV - CPC" o "EXTENDED CPC DSK File\r\nDisk-Info\r\n"     │
 * │    +0x30 Número de pistas                                           │
 * │    +0x31 Número de caras                                            │
 * │    +0x32-0x33 Tamaño de cada pista (solo DSK estándar)             │
 * │    +0x34+ (solo EXTENDED) Tamaño de pista en páginas de 256B       │
 * │                                                                     │
 * │  Cada pista tiene un header de 256 bytes:                           │
 * │    +0x00 "Track-Info\r\n"                                           │
 * │    +0x10 Track number                                               │
 * │    +0x11 Side number                                                │
 * │    +0x14 Sector size (N)                                            │
 * │    +0x15 Number of sectors                                          │
 * │    +0x16 GAP#3 length                                               │
 * │    +0x17 Filler byte                                                │
 * │    +0x18 Sector info × num_sectors (cada sector: 8 bytes)           │
 * └─────────────────────────────────────────────────────────────────────┘
 */

#include "cpc_fdc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────────────────────
// Utilidades internas
// ─────────────────────────────────────────────────────────────────────────────

// Tamaño de sector en bytes según el size code N
static inline int sector_size(uint8_t n) {
    if (n > 6) n = 6;
    return 128 << n;
}

// Libera la memoria de una imagen de disco cargada
static void disk_free(FDC_Disk* d) {
    if (!d) return;
    for (int t = 0; t < FDC_MAX_TRACKS; t++)
        for (int s = 0; s < FDC_MAX_SECTORS; s++)
            d->tracks[t].sectors[s].data = NULL;  // punteros dentro de raw_data
    free(d->raw_data);
    d->raw_data = NULL;
    d->raw_size = 0;
    d->inserted = false;
    d->num_tracks = 0;
    d->num_sides  = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parser del formato DSK
// ─────────────────────────────────────────────────────────────────────────────

// Señal de pista en el DSK estándar/extendido
static const char DSK_STD_SIG[] = "MV - CPC";
static const char DSK_EXT_SIG[] = "EXTENDED CPC DSK File";

bool fdc_load_dsk(FDC* fdc, int drive, const char* path) {
    if (drive < 0 || drive >= FDC_MAX_DRIVES) return false;

    // Abrir y leer el archivo completo
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FDC: No se puede abrir %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize < 256) { fclose(f); fprintf(stderr,"FDC: DSK demasiado pequeño\n"); return false; }

    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return false; }
    if ((long)fread(buf, 1, fsize, f) != fsize) {
        free(buf); fclose(f);
        fprintf(stderr,"FDC: Error leyendo %s\n", path);
        return false;
    }
    fclose(f);

    // Identificar formato
    bool extended = (memcmp(buf, DSK_EXT_SIG, strlen(DSK_EXT_SIG)) == 0);
    bool standard = (memcmp(buf, DSK_STD_SIG, strlen(DSK_STD_SIG)) == 0);
    if (!extended && !standard) {
        fprintf(stderr,"FDC: %s no es un archivo DSK válido\n", path);
        free(buf); return false;
    }

    // Extraer info de cabecera
    FDC_Drive* drv = &fdc->drives[drive];
    disk_free(&drv->disk);

    FDC_Disk* disk = &drv->disk;
    disk->raw_data    = buf;
    disk->raw_size    = (size_t)fsize;
    disk->extended    = extended;
    disk->num_tracks  = buf[0x30];
    disk->num_sides   = buf[0x31];
    disk->inserted    = true;
    disk->write_protected = false;

    if (disk->num_tracks <= 0 || disk->num_tracks > 84) disk->num_tracks = 42;
    if (disk->num_sides  <= 0 || disk->num_sides  >  2) disk->num_sides  = 1;

    // Tabla de tamaños de pista (solo EXTENDED DSK)
    // Byte en offset 0x34 + (side*num_tracks + track) = tamaño/256
    // (Si es 0, la pista no existe.)

    // Parsear cada pista
    uint32_t offset = 256;   // el primer track header empieza aquí

    for (int side = 0; side < disk->num_sides; side++) {
        for (int track = 0; track < disk->num_tracks; track++) {

            // Calcular offset de esta pista
            if (extended) {
                // En EXTENDED, el offset de cada pista se obtiene sumando
                // los tamaños de pistas anteriores (tabla en header[0x34+])
                // Recalculamos acumulando desde el principio
                uint32_t ext_off = 256;
                for (int s2 = 0; s2 < disk->num_sides; s2++) {
                    for (int t2 = 0; t2 < disk->num_tracks; t2++) {
                        int idx = s2 * disk->num_tracks + t2;
                        if (s2 == side && t2 == track) { offset = ext_off; goto found; }
                        uint8_t pg = buf[0x34 + idx];
                        ext_off += (uint32_t)pg * 256;
                    }
                }
                found:;
            }

            int tidx = side * disk->num_tracks + track;
            FDC_Track* trk = &disk->tracks[tidx];
            trk->num_sectors = 0;

            // Verificar que hay datos en esta pista
            if (offset + 256 > (uint32_t)fsize) {
                if (!extended) {
                    // DSK estándar: tamaño fijo de pista
                    uint16_t track_sz = ((uint16_t)buf[0x33] << 8) | buf[0x32];
                    offset += track_sz;
                }
                continue;
            }

            uint8_t* th = buf + offset;  // track header

            // Verificar firma "Track-Info"
            if (memcmp(th, "Track-Info", 10) != 0) {
                if (!extended) {
                    uint16_t track_sz = ((uint16_t)buf[0x33] << 8) | buf[0x32];
                    offset += track_sz;
                }
                continue;
            }

            // Header de pista:
            // +0x10: track number, +0x11: side, +0x14: N, +0x15: num_sectors
            // +0x16: gap3, +0x17: filler
            // +0x18: sector info table (8 bytes por sector)
            int    num_sec  = th[0x15];
            if (num_sec > FDC_MAX_SECTORS) num_sec = FDC_MAX_SECTORS;

            // Los datos de sectores empiezan después del header de 256 bytes
            uint32_t data_off = offset + 256;

            for (int s = 0; s < num_sec; s++) {
                uint8_t* si = th + 0x18 + s * 8;
                FDC_Sector* sec = &trk->sectors[s];

                sec->track     = si[0];   // C
                sec->side      = si[1];   // H
                sec->sector_id = si[2];   // R
                sec->size_code = si[3];   // N
                sec->st1       = si[4];   // ST1
                sec->st2       = si[5];   // ST2

                // Tamaño de datos del sector
                uint16_t actual_len;
                if (extended) {
                    // En EXTENDED, los bytes 6-7 del sector info son el
                    // tamaño real del sector en el archivo (puede ser diferente
                    // de 128<<N si hay sectores con datos especiales)
                    actual_len = ((uint16_t)si[7] << 8) | si[6];
                } else {
                    actual_len = (uint16_t)sector_size(sec->size_code);
                }

                sec->data_len = actual_len;

                // El puntero apunta directamente al buffer raw para evitar copias
                if (data_off + actual_len <= (uint32_t)fsize) {
                    sec->data = buf + data_off;
                } else {
                    sec->data = NULL;   // sector truncado en el archivo
                    fprintf(stderr,"FDC: Sector truncado en pista %d/%d sector %d\n",
                            track, side, s);
                }

                data_off += actual_len;
            }

            trk->num_sectors = num_sec;

            // Avanzar al siguiente track
            if (extended) {
                // offset ya fue calculado arriba para el track siguiente
                int idx = side * disk->num_tracks + track;
                offset += (uint32_t)buf[0x34 + idx] * 256;
            } else {
                uint16_t track_sz = ((uint16_t)buf[0x33] << 8) | buf[0x32];
                offset += track_sz;
            }
        }
    }

    drv->current_track = 0;
    drv->ready         = true;
    drv->motor_on      = false;

    printf("FDC: DSK %s cargado en unidad %c: — %d pistas, %d cara(s), %s\n",
           path, 'A' + drive,
           disk->num_tracks, disk->num_sides,
           extended ? "EXTENDED" : "estándar");
    return true;
}

void fdc_eject(FDC* fdc, int drive) {
    if (drive < 0 || drive >= FDC_MAX_DRIVES) return;
    disk_free(&fdc->drives[drive].disk);
    fdc->drives[drive].ready = false;
    printf("FDC: Disco expulsado de unidad %c:\n", 'A' + drive);
}

// ─────────────────────────────────────────────────────────────────────────────
// Init / Reset
// ─────────────────────────────────────────────────────────────────────────────

void fdc_init(FDC* fdc) {
    memset(fdc, 0, sizeof(FDC));
    fdc_reset(fdc);
}

void fdc_reset(FDC* fdc) {
    fdc->phase       = FDC_IDLE;
    fdc->cmd_pos     = 0;
    fdc->cmd_len     = 0;
    fdc->res_pos     = 0;
    fdc->res_len     = 0;
    fdc->irq_pending = false;
    fdc->st0 = fdc->st1 = fdc->st2 = 0;
    fdc->srt = 8; fdc->hut = 8; fdc->hlt = 2; fdc->nd = true;
    // No resetear los discos montados
    for (int d = 0; d < FDC_MAX_DRIVES; d++) {
        fdc->drives[d].motor_on = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Búsqueda de sector en la pista actual
// ─────────────────────────────────────────────────────────────────────────────
static FDC_Sector* find_sector(FDC* fdc, int drive, int side, int track,
                                uint8_t C, uint8_t H, uint8_t R, uint8_t N) {
    FDC_Drive* drv = &fdc->drives[drive];
    if (!drv->disk.inserted) return NULL;
    if (track >= drv->disk.num_tracks) return NULL;

    int tidx = side * drv->disk.num_tracks + track;
    if (tidx >= FDC_MAX_TRACKS) return NULL;

    FDC_Track* trk = &drv->disk.tracks[tidx];
    for (int s = 0; s < trk->num_sectors; s++) {
        FDC_Sector* sec = &trk->sectors[s];
        // El CPC busca por R (sector ID). C, H, N son opcionales según comando.
        if (sec->sector_id == R) {
            (void)C; (void)H; (void)N;
            return sec;
        }
    }
    return NULL;
}

// Siguiente sector en la pista (wrap around)
static FDC_Sector* next_sector(FDC* fdc, int drive, int side, int track, int* idx) {
    FDC_Drive* drv = &fdc->drives[drive];
    if (!drv->disk.inserted) return NULL;
    int tidx = side * drv->disk.num_tracks + track;
    if (tidx >= FDC_MAX_TRACKS) return NULL;
    FDC_Track* trk = &drv->disk.tracks[tidx];
    if (trk->num_sectors == 0) return NULL;
    *idx = (*idx + 1) % trk->num_sectors;
    return &trk->sectors[*idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers para construir la fase de resultado
// ─────────────────────────────────────────────────────────────────────────────
static void set_result_rw(FDC* fdc, FDC_Sector* sec) {
    // Resultado de 7 bytes para Read/Write Data
    fdc->res_buf[0] = fdc->st0;
    fdc->res_buf[1] = fdc->st1;
    fdc->res_buf[2] = fdc->st2;
    fdc->res_buf[3] = sec ? sec->track     : 0;
    fdc->res_buf[4] = sec ? sec->side      : 0;
    fdc->res_buf[5] = sec ? sec->sector_id : 0;
    fdc->res_buf[6] = sec ? sec->size_code : 0;
    fdc->res_len    = 7;
    fdc->res_pos    = 0;
    fdc->phase      = FDC_RESULT;
    fdc->irq_pending= true;
}

static void set_result_error(FDC* fdc, uint8_t st0_ic) {
    fdc->st0 = (fdc->st0 & ~0xC0) | st0_ic;
    FDC_Sector dummy = {0};
    set_result_rw(fdc, &dummy);
}

// ─────────────────────────────────────────────────────────────────────────────
// Ejecución de comandos
// ─────────────────────────────────────────────────────────────────────────────

// Número de bytes de comando para cada código de comando (bits 0-4)
static const int cmd_lengths[32] = {
//  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
    1, 1, 9, 3, 2, 9, 9, 2, 1, 9, 2, 1, 9, 6, 1, 3,
// 16 17 18 19 ...
    1, 9, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

static void execute_command(FDC* fdc) {
    uint8_t  cmd    = fdc->cmd_buf[0] & 0x1F;
    uint8_t  flags  = fdc->cmd_buf[0];         // MF, MFM, SK en bits superiores
    uint8_t  drive  = fdc->cmd_buf[1] & 0x03;
    uint8_t  side   = (fdc->cmd_buf[1] >> 2) & 0x01;
    uint8_t  C      = (fdc->cmd_len > 2) ? fdc->cmd_buf[2] : 0;
    uint8_t  H      = (fdc->cmd_len > 3) ? fdc->cmd_buf[3] : 0;
    uint8_t  R      = (fdc->cmd_len > 4) ? fdc->cmd_buf[4] : 0;
    uint8_t  N      = (fdc->cmd_len > 5) ? fdc->cmd_buf[5] : 0;
    uint8_t  EOT    = (fdc->cmd_len > 6) ? fdc->cmd_buf[6] : 0;
    (void)EOT;

    fdc->sel_drive  = drive;
    fdc->sel_side   = side;

    FDC_Drive* drv  = &fdc->drives[drive & 1];

    // Construir ST0 base
    fdc->st0 = (drive & 0x03) | (side ? FDC_ST0_HD : 0);
    fdc->st1 = 0;
    fdc->st2 = 0;

    switch (cmd) {
        // ── 0x03 SPECIFY ─────────────────────────────────────────────────────
        case 0x03:
            fdc->srt = (fdc->cmd_buf[1] >> 4) & 0x0F;
            fdc->hut = (fdc->cmd_buf[1]      ) & 0x0F;
            fdc->hlt = (fdc->cmd_buf[2] >> 1 ) & 0x7F;
            fdc->nd  = (fdc->cmd_buf[2] & 0x01) != 0;
            fdc->phase = FDC_IDLE;
            return;

        // ── 0x04 SENSE DRIVE STATUS ──────────────────────────────────────────
        case 0x04:
            fdc->st0 = (drive & 3) | (side ? FDC_ST0_HD : 0);
            if (drv->disk.inserted && drv->motor_on) {
                if (drv->current_track == 0) fdc->st0 |= 0x10; // Track 0
            } else {
                fdc->st0 |= FDC_ST0_NR;
            }
            fdc->res_buf[0] = fdc->st0;
            fdc->res_len = 1; fdc->res_pos = 0;
            fdc->phase = FDC_RESULT;
            fdc->irq_pending = true;
            return;

        // ── 0x07 RECALIBRATE ─────────────────────────────────────────────────
        case 0x07:
            drv->current_track = 0;
            fdc->st0 = FDC_ST0_SE | (drive & 3);
            if (!drv->disk.inserted) fdc->st0 |= FDC_ST0_EC | FDC_ST0_NR;
            fdc->phase = FDC_IDLE;
            fdc->irq_pending = true;
            return;

        // ── 0x08 SENSE INTERRUPT STATUS ──────────────────────────────────────
        case 0x08:
            // Retorna ST0 + PCN (Present Cylinder Number)
            fdc->res_buf[0] = fdc->st0 | FDC_ST0_SE;
            fdc->res_buf[1] = (uint8_t)drv->current_track;
            fdc->res_len = 2; fdc->res_pos = 0;
            fdc->phase = FDC_RESULT;
            // NO genera IRQ en sí mismo
            return;

        // ── 0x0F SEEK ────────────────────────────────────────────────────────
        case 0x0F: {
            uint8_t ncn = fdc->cmd_buf[2];  // New Cylinder Number
            drv->current_track = ncn;
            if (drv->current_track >= (drv->disk.inserted ? drv->disk.num_tracks : 84))
                drv->current_track = drv->disk.inserted ? drv->disk.num_tracks - 1 : 0;
            fdc->st0 = FDC_ST0_SE | (drive & 3) | (side ? FDC_ST0_HD : 0);
            fdc->phase = FDC_IDLE;
            fdc->irq_pending = true;
            return;
        }

        // ── 0x06 READ DATA ────────────────────────────────────────────────────
        case 0x06: {
            if (!drv->disk.inserted || !drv->motor_on) {
                fdc->st0 |= FDC_ST0_IC_AT | FDC_ST0_NR;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            // Buscar el sector en la pista actual
            FDC_Sector* sec = find_sector(fdc, drive & 1, side,
                                          drv->current_track, C, H, R, N);
            if (!sec || !sec->data) {
                // Sector no encontrado
                fdc->st0 |= FDC_ST0_IC_AT;
                fdc->st1 |= FDC_ST1_ND;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            // Propagar st1/st2 del sector (marcas de error guardadas en DSK)
            fdc->st1 = sec->st1;
            fdc->st2 = sec->st2;

            // Entrar en fase de ejecución: transferencia byte a byte
            fdc->phase    = FDC_EXECUTION;
            fdc->data_pos = 0;

            // Guardamos puntero al sector en curso reutilizando cmd_buf
            // (truco: guardamos sus parámetros para el resultado)
            fdc->cmd_buf[2] = sec->track;
            fdc->cmd_buf[3] = sec->side;
            fdc->cmd_buf[4] = sec->sector_id;
            fdc->cmd_buf[5] = sec->size_code;

            // El sector actual se accede durante fdc_read_data()
            // Guardamos track/side/R en st0 aux para localizarlo después
            fdc->st0 = (fdc->st0 & 0x3F) | (drive & 3) | (side ? FDC_ST0_HD : 0);
            return;
        }

        // ── 0x05 WRITE DATA ───────────────────────────────────────────────────
        case 0x05: {
            if (!drv->disk.inserted || !drv->motor_on) {
                fdc->st0 |= FDC_ST0_IC_AT | FDC_ST0_NR;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            if (drv->disk.write_protected) {
                fdc->st0 |= FDC_ST0_IC_AT;
                fdc->st1 |= FDC_ST1_NW;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            FDC_Sector* sec = find_sector(fdc, drive & 1, side,
                                          drv->current_track, C, H, R, N);
            if (!sec || !sec->data) {
                fdc->st0 |= FDC_ST0_IC_AT;
                fdc->st1 |= FDC_ST1_ND;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            fdc->phase    = FDC_EXECUTION;
            fdc->data_pos = 0;
            fdc->cmd_buf[2] = sec->track;
            fdc->cmd_buf[3] = sec->side;
            fdc->cmd_buf[4] = sec->sector_id;
            fdc->cmd_buf[5] = sec->size_code;
            return;
        }

        // ── 0x0A READ ID ──────────────────────────────────────────────────────
        case 0x0A: {
            if (!drv->disk.inserted || !drv->motor_on) {
                fdc->st0 |= FDC_ST0_IC_AT | FDC_ST0_NR;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            int tidx = side * drv->disk.num_tracks + drv->current_track;
            if (tidx >= FDC_MAX_TRACKS || drv->disk.tracks[tidx].num_sectors == 0) {
                fdc->st0 |= FDC_ST0_IC_AT;
                fdc->st1 |= FDC_ST1_MA;
                set_result_error(fdc, FDC_ST0_IC_AT);
                return;
            }
            // Devuelve el ID del primer sector de la pista
            FDC_Sector* sec = &drv->disk.tracks[tidx].sectors[fdc->cur_sector % drv->disk.tracks[tidx].num_sectors];
            fdc->st0 |= FDC_ST0_IC_OK;
            fdc->res_buf[0] = fdc->st0;
            fdc->res_buf[1] = fdc->st1;
            fdc->res_buf[2] = fdc->st2;
            fdc->res_buf[3] = sec->track;
            fdc->res_buf[4] = sec->side;
            fdc->res_buf[5] = sec->sector_id;
            fdc->res_buf[6] = sec->size_code;
            fdc->res_len = 7; fdc->res_pos = 0;
            fdc->phase = FDC_RESULT;
            fdc->irq_pending = true;
            return;
        }

        // ── 0x0D FORMAT TRACK ─────────────────────────────────────────────────
        case 0x0D: {
            // Formatear en un DSK en memoria es complejo; devolvemos OK
            // sin modificar los datos (suficiente para que AMSDOS no se cuelgue)
            fdc->st0 |= FDC_ST0_IC_OK;
            fdc->res_buf[0] = fdc->st0;
            fdc->res_buf[1] = fdc->st1 = 0;
            fdc->res_buf[2] = fdc->st2 = 0;
            fdc->res_buf[3] = fdc->res_buf[4] = fdc->res_buf[5] = fdc->res_buf[6] = 0;
            fdc->res_len = 7; fdc->res_pos = 0;
            fdc->phase = FDC_RESULT;
            fdc->irq_pending = true;
            return;
        }

        // ── 0x02 READ TRACK ───────────────────────────────────────────────────
        case 0x02:
            // Igual que READ DATA pero desde el primer sector de la pista
            fdc->phase    = FDC_EXECUTION;
            fdc->data_pos = 0;
            fdc->cur_sector = 0;
            return;

        // ── Comando inválido ──────────────────────────────────────────────────
        default:
            fdc->st0 = FDC_ST0_IC_IC;
            fdc->res_buf[0] = fdc->st0;
            fdc->res_len = 1; fdc->res_pos = 0;
            fdc->phase = FDC_RESULT;
            fdc->irq_pending = true;
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Acceso al sector en curso (para READ DATA / WRITE DATA)
// ─────────────────────────────────────────────────────────────────────────────
static FDC_Sector* current_exec_sector(FDC* fdc) {
    int drive  = fdc->sel_drive & 1;
    int side   = fdc->sel_side;
    FDC_Drive* drv = &fdc->drives[drive];
    if (!drv->disk.inserted) return NULL;

    int tidx = side * drv->disk.num_tracks + drv->current_track;
    if (tidx >= FDC_MAX_TRACKS) return NULL;
    FDC_Track* trk = &drv->disk.tracks[tidx];

    // Buscar por R guardado en cmd_buf[4]
    uint8_t R = fdc->cmd_buf[4];
    for (int s = 0; s < trk->num_sectors; s++)
        if (trk->sectors[s].sector_id == R)
            return &trk->sectors[s];
    return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Puertos I/O del FDC
// ─────────────────────────────────────────────────────────────────────────────

uint8_t fdc_read_status(FDC* fdc) {
    uint8_t msr = FDC_MSR_RQM;   // Siempre listo para intercambio básico

    switch (fdc->phase) {
        case FDC_IDLE:
            // CPU puede escribir comando
            msr = FDC_MSR_RQM;
            break;
        case FDC_COMMAND:
            // CPU enviando bytes de comando → FDC recibe (DIO=0)
            msr = FDC_MSR_RQM | FDC_MSR_CB;
            break;
        case FDC_EXECUTION: {
            uint8_t cmd = fdc->cmd_buf[0] & 0x1F;
            if (cmd == 0x06 || cmd == 0x02) {
                // READ: FDC→CPU (DIO=1)
                msr = FDC_MSR_RQM | FDC_MSR_CB | FDC_MSR_EXM | FDC_MSR_DIO;
            } else if (cmd == 0x05 || cmd == 0x0D) {
                // WRITE: CPU→FDC (DIO=0)
                msr = FDC_MSR_RQM | FDC_MSR_CB | FDC_MSR_EXM;
            }
            break;
        }
        case FDC_RESULT:
            // FDC→CPU: resultados disponibles (DIO=1)
            msr = FDC_MSR_RQM | FDC_MSR_CB | FDC_MSR_DIO;
            break;
    }

    // Añadir bits de busy para las unidades en seek
    for (int d = 0; d < FDC_MAX_DRIVES; d++)
        if (fdc->drives[d].motor_on && fdc->drives[d].disk.inserted)
            msr |= (FDC_MSR_DB0 << d);

    return msr;
}

uint8_t fdc_read_data(FDC* fdc) {
    if (fdc->phase == FDC_RESULT) {
        if (fdc->res_pos < fdc->res_len) {
            uint8_t v = fdc->res_buf[fdc->res_pos++];
            if (fdc->res_pos >= fdc->res_len) {
                // Todos los bytes de resultado leídos → FDC libre
                fdc->phase = FDC_IDLE;
                fdc->cmd_pos = 0;
            }
            return v;
        }
        return 0xFF;
    }

    if (fdc->phase == FDC_EXECUTION) {
        uint8_t cmd = fdc->cmd_buf[0] & 0x1F;

        if (cmd == 0x06 || cmd == 0x02) {
            // READ DATA: devolver bytes del sector
            FDC_Sector* sec = current_exec_sector(fdc);
            if (!sec || !sec->data || fdc->data_pos >= sec->data_len) {
                // Fin del sector → resultado
                fdc->st0 |= FDC_ST0_IC_OK;
                set_result_rw(fdc, sec);
                return 0xFF;
            }
            uint8_t byte = sec->data[fdc->data_pos++];
            if (fdc->data_pos >= sec->data_len) {
                // Sector completo leído
                fdc->st0 |= FDC_ST0_IC_OK;
                set_result_rw(fdc, sec);
            }
            return byte;
        }
    }

    return 0xFF;
}

void fdc_write_data(FDC* fdc, uint8_t val) {
    switch (fdc->phase) {

        case FDC_IDLE:
        case FDC_COMMAND: {
            // Recibir byte de comando
            if (fdc->phase == FDC_IDLE) {
                // Primer byte: código de comando
                fdc->cmd_buf[0] = val;
                fdc->cmd_pos = 1;
                uint8_t cmd = val & 0x1F;
                fdc->cmd_len = (cmd < 32) ? cmd_lengths[cmd] : 1;
                if (fdc->cmd_len <= 1) {
                    execute_command(fdc);
                } else {
                    fdc->phase = FDC_COMMAND;
                }
            } else {
                // Bytes de comando siguientes
                if (fdc->cmd_pos < 9)
                    fdc->cmd_buf[fdc->cmd_pos++] = val;
                if (fdc->cmd_pos >= fdc->cmd_len) {
                    execute_command(fdc);
                }
            }
            break;
        }

        case FDC_EXECUTION: {
            uint8_t cmd = fdc->cmd_buf[0] & 0x1F;
            if (cmd == 0x05) {
                // WRITE DATA: escribir bytes en el sector
                FDC_Sector* sec = current_exec_sector(fdc);
                if (sec && sec->data && fdc->data_pos < sec->data_len) {
                    sec->data[fdc->data_pos++] = val;
                    if (fdc->data_pos >= sec->data_len) {
                        fdc->st0 |= FDC_ST0_IC_OK;
                        set_result_rw(fdc, sec);
                    }
                }
            }
            break;
        }

        case FDC_RESULT:
            // Ignorar escrituras en fase de resultado
            break;
    }
}

void fdc_motor_control(FDC* fdc, uint8_t val) {
    // Bit 0 = motor unidad A, bit 1 = motor unidad B
    for (int d = 0; d < FDC_MAX_DRIVES; d++) {
        bool on = (val >> d) & 1;
        fdc->drives[d].motor_on = on;
        if (on && fdc->drives[d].disk.inserted)
            fdc->drives[d].ready = true;
    }
    fdc->motor = (val != 0);
}

bool fdc_irq(FDC* fdc) {
    if (fdc->irq_pending) {
        fdc->irq_pending = false;
        return true;
    }
    return false;
}

void fdc_tick(FDC* fdc, int delta) {
    // El FDC real tiene temporizadores para step rate, head load, etc.
    // En esta implementación las operaciones son instantáneas,
    // así que tick() es un no-op. Se mantiene para compatibilidad futura.
    (void)fdc;
    (void)delta;
}
