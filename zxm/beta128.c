/*
 * beta128.c – Emulación Beta 128 Disk Interface con WD1793 FDC
 *
 * Implementa el controlador WD1793 y las funciones de carga de
 * imágenes TRD y SCL para emular el sistema de disco TR-DOS
 * usado en Pentagon, Scorpion y otros clones rusos del ZX Spectrum.
 */

#include "beta128.h"
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers internos
// ─────────────────────────────────────────────────────────────────────────────

static BetaDrive* cur_drive(Beta128* beta) {
    return &beta->drives[beta->sel_drive & (BETA_MAX_DRIVES - 1)];
}

// Offset en el buffer de datos del disco para una posición (track, side, sector)
// Sectores numerados 1..16, tracks 0..79, sides 0..1
static int disk_offset(int track, int side, int sector) {
    // Disposición TRD: side 0 track 0, side 1 track 0, side 0 track 1, ...
    int logical_track = track * 2 + side;
    return (logical_track * BETA_SECTORS_PER_TRACK + (sector - 1)) * BETA_SECTOR_SIZE;
}

static uint8_t* sector_ptr(Beta128* beta, int track, int side, int sector) {
    BetaDrive* drv = cur_drive(beta);
    if (!drv->disk.inserted || !drv->disk.data) return NULL;
    if (track < 0 || track >= drv->disk.num_tracks) return NULL;
    if (side < 0 || side >= drv->disk.num_sides) return NULL;
    if (sector < 1 || sector > BETA_SECTORS_PER_TRACK) return NULL;

    int off = disk_offset(track, side, sector);
    if (off < 0 || (size_t)(off + BETA_SECTOR_SIZE) > drv->disk.data_size) return NULL;
    return &drv->disk.data[off];
}

// ─────────────────────────────────────────────────────────────────────────────
// Inicialización / Reset
// ─────────────────────────────────────────────────────────────────────────────

void beta128_init(Beta128* beta) {
    memset(beta, 0, sizeof(Beta128));
    beta->step_dir = 1;
    beta->sel_drive = 0;
    beta->sel_side = 0;
    for (int d = 0; d < BETA_MAX_DRIVES; d++) {
        beta->drives[d].current_track = 0;
        beta->drives[d].motor_on = false;
        beta->drives[d].disk.inserted = false;
    }
}

void beta128_reset(Beta128* beta) {
    // Based on ESPectrum rvmWD1793Reset()
    beta->status = 0;
    beta->track_reg = 0;
    beta->sector_reg = 1;
    beta->data_reg = 0;
    beta->command_reg = 0;
    beta->system_reg = 0;
    beta->cmd_type = WD_CMD_NONE;
    beta->busy = false;
    beta->drq = false;
    beta->intrq = false;
    beta->fintrq = false;
    beta->status_type1 = true;  // Power-on status is Type I
    beta->head_loaded = false;
    beta->data_pos = 0;
    beta->data_len = 0;
    beta->data_ptr = NULL;
    beta->data_write = false;
    beta->multi_sector = false;
    beta->step_dir = 1;
    beta->sel_drive = 0;
    beta->sel_side = 0;
    beta->index_counter = 0;
    beta->index_pulse = true;  // Disk is already spinning on power-up
    // No resetear discos montados
}

// ─────────────────────────────────────────────────────────────────────────────
// Ejecución de comandos WD1793
// ─────────────────────────────────────────────────────────────────────────────

// Based on ESPectrum _end()
static void finish_command(Beta128* beta, uint8_t status_bits) {
    beta->busy = false;
    beta->drq = false;
    beta->intrq = true;
    beta->status = status_bits;
    beta->cmd_type = WD_CMD_NONE;
    // status_type1 stays as the command set it (Type I commands set it to
    // true; Type II/III set it to false).  This is important because
    // TRACK0 (bit 2 in Type I) shares the same bit as LOST_DATA (bit 2 in
    // Type II/III) — forcing Type I after READ SECTOR at track 0 would
    // cause TR-DOS to misinterpret TRACK0 as a disk error.

    BetaDrive* drv = cur_drive(beta);
    if (!drv->disk.inserted) {
        beta->status |= WD_ST_NOTREADY;
    }
}

// Type I commands — based on ESPectrum state machine.
// ESPectrum sets kRVMWD177XStatusSetHead at TypeIEnd, then _end() sets INTRQ.
// Status bits Track0 and WP are resolved dynamically at read time.
//
// Type I commands use cmd_delay to simulate seek/step time.  This is
// critical for Scorpion: TR-DOS's ISR (at $FFEF) corrupts register A.
// If RESTORE completes instantly, the ISR fires at a point where the
// corruption causes a wrong code path and premature exit.  With a
// realistic delay, the ISR fires during the busy-wait polling loop
// where A is immediately reloaded by IN A,($1F), so corruption is
// harmless.
//
// Step rate per ESPectrum: srate[5][0]=46 steps, WD177XSTEPSTATES=112
// → ~5152 T-states per track step at fastest rate.

#define WD_STEP_TSTATES 5152  // T-states per track step (fastest rate)

static void exec_restore(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);
    int steps = drv->current_track > 0 ? drv->current_track : 1;
    drv->current_track = 0;
    beta->track_reg = 0;
    beta->status_type1 = true;
    beta->busy = true;
    beta->status = WD_ST_BUSY;
    beta->cmd_delay = steps * WD_STEP_TSTATES;
    beta->drq = false;
}

static void exec_seek(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);
    int target = beta->data_reg;
    if (target < 0) target = 0;
    if (target >= BETA_TRACKS_PER_SIDE) target = BETA_TRACKS_PER_SIDE - 1;

    if (target > drv->current_track) beta->step_dir = 1;
    else if (target < drv->current_track) beta->step_dir = -1;

    int steps = abs(target - drv->current_track);
    if (steps < 1) steps = 1;
    drv->current_track = target;
    beta->track_reg = (uint8_t)target;
    beta->status_type1 = true;
    beta->busy = true;
    beta->status = WD_ST_BUSY;
    beta->cmd_delay = steps * WD_STEP_TSTATES;
    beta->drq = false;
}

static void exec_step(Beta128* beta, int direction) {
    BetaDrive* drv = cur_drive(beta);

    if (direction != 0) beta->step_dir = direction;
    int new_track = drv->current_track + beta->step_dir;
    if (new_track < 0) new_track = 0;
    if (new_track >= BETA_TRACKS_PER_SIDE) new_track = BETA_TRACKS_PER_SIDE - 1;

    drv->current_track = new_track;

    if (beta->command_reg & 0x10) {
        beta->track_reg = (uint8_t)new_track;
    }

    beta->status_type1 = true;
    beta->busy = true;
    beta->status = WD_ST_BUSY;
    beta->cmd_delay = WD_STEP_TSTATES;
    beta->drq = false;
}

static void exec_read_sector(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);

    if (!drv->disk.inserted) {
        finish_command(beta, WD_ST_RNFERR);
        return;
    }

    uint8_t* ptr = sector_ptr(beta, drv->current_track, beta->sel_side,
                              beta->sector_reg);
    if (!ptr) {
        finish_command(beta, WD_ST_RNFERR);
        return;
    }

    beta->data_ptr = ptr;
    beta->data_pos = 0;
    beta->data_len = BETA_SECTOR_SIZE;
    beta->data_write = false;
    beta->drq = true;
    beta->drq_timer = 0;
    beta->busy = true;
    beta->status = WD_ST_BUSY;  // DRQ resolved dynamically on status read
}

static void exec_write_sector(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);

    if (!drv->disk.inserted) {
        finish_command(beta, WD_ST_RNFERR);
        return;
    }

    if (drv->disk.write_protected) {
        finish_command(beta, WD_ST_WRPROTECT);
        return;
    }

    uint8_t* ptr = sector_ptr(beta, drv->current_track, beta->sel_side,
                              beta->sector_reg);
    if (!ptr) {
        finish_command(beta, WD_ST_RNFERR);
        return;
    }

    beta->data_ptr = ptr;
    beta->data_pos = 0;
    beta->data_len = BETA_SECTOR_SIZE;
    beta->data_write = true;
    beta->drq = true;
    beta->busy = true;
    beta->status = WD_ST_BUSY;  // DRQ resolved dynamically on status read
}

static void exec_read_address(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);

    if (!drv->disk.inserted) {
        finish_command(beta, WD_ST_RNFERR);
        return;
    }

    // Simulate disk rotation: return incrementing sector IDs (1-16)
    beta->rot_sector++;
    if (beta->rot_sector > BETA_SECTORS_PER_TRACK)
        beta->rot_sector = 1;

    // Prepare 6 bytes: track, side, sector, sector_size, crc1, crc2
    beta->id_buf[0] = (uint8_t)drv->current_track;
    beta->id_buf[1] = beta->sel_side;
    beta->id_buf[2] = (uint8_t)beta->rot_sector;
    beta->id_buf[3] = 0x01; // size code 1 = 256 bytes
    beta->id_buf[4] = 0x00; // CRC placeholder
    beta->id_buf[5] = 0x00;

    // Also update sector_reg (ESPectrum behavior)
    beta->sector_reg = (uint8_t)beta->rot_sector;

    // Assert DRQ immediately — matches YASE/ESPectrum instant completion
    // model. The host reads 6 bytes via the data register; DRQ is
    // reasserted after each byte read.
    beta->data_ptr = beta->id_buf;
    beta->data_pos = 0;
    beta->data_len = 6;
    beta->data_write = false;
    beta->drq = true;
    beta->drq_timer = 0;
    beta->cmd_delay = 0;
    beta->busy = true;
    beta->status = WD_ST_BUSY;
}

// Based on ESPectrum rvmWD1793Write() command handling
static void exec_command(Beta128* beta, uint8_t cmd) {
    beta->command_reg = cmd;

    // Type IV: Force Interrupt — based on ESPectrum
    if ((cmd & 0xF0) == 0xD0) {
        beta->cmd_type = WD_CMD_FORCE_INT;

        if (beta->busy) {
            // Abort current command (ESPectrum: _end())
            beta->busy = false;
            beta->drq = false;
            beta->cmd_delay = 0;
            beta->data_ptr = NULL;
            beta->data_pos = 0;
            beta->data_len = 0;
            // Reset to Type I status so HEAD_LOADED is visible
            beta->status_type1 = true;
            beta->status = 0;
        } else {
            // Not busy: set Type I status with dynamic flags
            beta->status_type1 = true;
            beta->status = 0;
        }

        // Lower nibble controls INTRQ (ESPectrum behavior)
        if ((cmd & 0x0F) == 0x00) {
            // $D0: clear both INTRQ sources
            beta->intrq = false;
            beta->fintrq = false;
        } else if (cmd & 0x08) {
            // Bit 3 set: immediate INTRQ
            beta->fintrq = true;
        } else {
            beta->intrq = true;
        }
        return;
    }

    // Don't accept new commands while busy (ESPectrum check)
    if (beta->busy) return;

    // Clear INTRQ on new command (ESPectrum behavior)
    beta->intrq = false;
    beta->fintrq = false;

    // Check if drive has disk (ESPectrum: check disk exists and power)
    BetaDrive* drv = cur_drive(beta);

    // Type I: RESTORE, SEEK, STEP, STEP-IN, STEP-OUT
    if ((cmd & 0x80) == 0x00) {

        beta->busy = true;
        beta->status = WD_ST_BUSY;
        beta->status_type1 = true;

        if ((cmd & 0xF0) == 0x00) {
            beta->cmd_type = WD_CMD_RESTORE;
            exec_restore(beta);
        } else if ((cmd & 0xF0) == 0x10) {
            beta->cmd_type = WD_CMD_SEEK;
            exec_seek(beta);
        } else if ((cmd & 0xE0) == 0x20) {
            beta->cmd_type = WD_CMD_STEP;
            exec_step(beta, 0);
        } else if ((cmd & 0xE0) == 0x40) {
            beta->cmd_type = WD_CMD_STEP_IN;
            exec_step(beta, 1);
        } else if ((cmd & 0xE0) == 0x60) {
            beta->cmd_type = WD_CMD_STEP_OUT;
            exec_step(beta, -1);
        }
        return;
    }

    // Type II: READ SECTOR, WRITE SECTOR
    if ((cmd & 0xC0) == 0x80) {

        beta->status_type1 = false;
        beta->multi_sector = (cmd & 0x10) ? true : false;

        if ((cmd & 0xE0) == 0x80) {
            beta->cmd_type = WD_CMD_READ_SEC;
            exec_read_sector(beta);
        } else {
            beta->cmd_type = WD_CMD_WRITE_SEC;
            exec_write_sector(beta);
        }
        return;
    }

    // Type III: READ ADDRESS, READ TRACK, WRITE TRACK
    if ((cmd & 0xF0) == 0xC0) {
        beta->status_type1 = false;
        beta->cmd_type = WD_CMD_READ_ADDR;
        exec_read_address(beta);
        return;
    }

    if ((cmd & 0xF0) == 0xE0) {
        beta->status_type1 = false;
        // READ TRACK - simplified: treat as reading the whole track
        beta->cmd_type = WD_CMD_READ_TRACK;
        if (!drv->disk.inserted) {
            finish_command(beta, WD_ST_RNFERR);
            return;
        }
        // Point to the beginning of the track
        int off = disk_offset(drv->current_track, beta->sel_side, 1);
        if (off >= 0 && (size_t)(off + BETA_TRACK_SIZE) <= drv->disk.data_size) {
            beta->data_ptr = &drv->disk.data[off];
            beta->data_pos = 0;
            beta->data_len = BETA_TRACK_SIZE;
            beta->data_write = false;
            beta->drq = true;
            beta->busy = true;
            beta->status = WD_ST_BUSY;
        } else {
            finish_command(beta, WD_ST_RNFERR);
        }
        return;
    }

    if ((cmd & 0xF0) == 0xF0) {
        beta->status_type1 = false;
        // WRITE TRACK (format) — complete immediately (disk pre-formatted)
        beta->cmd_type = WD_CMD_WRITE_TRACK;
        if (!drv->disk.inserted) {
            finish_command(beta, WD_ST_RNFERR);
            return;
        }
        if (drv->disk.write_protected) {
            finish_command(beta, WD_ST_WRPROTECT);
            return;
        }
        finish_command(beta, 0);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lectura / Escritura de puertos
// ─────────────────────────────────────────────────────────────────────────────

// Based on ESPectrum rvmWD1793Read()
uint8_t beta128_read(Beta128* beta, uint16_t port) {
    switch (port & 0xFF) {
        case 0x1F: {
            // Status Register — based on ESPectrum dynamic flag resolution
            beta->intrq = false;  // Reading status clears INTRQ
            beta->fintrq = false; // Also clear forced INTRQ

            uint8_t st = beta->status & 0xFF;
            BetaDrive* drv = cur_drive(beta);

            if (beta->status_type1) {
                // Type I status: dynamically resolve Track0, WP, Index, HeadLoaded
                if (drv->disk.write_protected)
                    st |= WD_ST_WRPROTECT;

                if (drv->current_track == 0)
                    st |= WD_ST_TRACK0;

                // Index pulse: simulated rotation (ESPectrum: kRVMwdDiskOutIndex)
                if (beta->index_pulse)
                    st |= WD_ST_INDEX;

                // Head loaded (ESPectrum: kRVMWD177XStatusSetHead)
                if (beta->head_loaded)
                    st |= WD_ST_HEADLOADED;
            } else {
                // Type II/III status: DRQ is in bit 1
                if (beta->drq)
                    st |= WD_ST_DRQ;
                // NOTREADY only if no disk (already handled above)
            }

            return st;
        }

        case 0x3F:
            return beta->track_reg;

        case 0x5F:
            return beta->sector_reg;

        case 0x7F: {
            // Data Register — based on ESPectrum: clear DRQ on read
            beta->drq = false;

            if (beta->data_ptr && !beta->data_write) {
                if (beta->data_pos < beta->data_len) {
                    beta->data_reg = beta->data_ptr[beta->data_pos++];

                    if (beta->data_pos >= beta->data_len) {
                        if (beta->multi_sector && beta->cmd_type == WD_CMD_READ_SEC) {
                            beta->sector_reg++;
                            if (beta->sector_reg <= BETA_SECTORS_PER_TRACK) {
                                exec_read_sector(beta);
                            } else {
                                finish_command(beta, WD_ST_RNFERR);
                            }
                        } else {
                            finish_command(beta, 0);
                        }
                    } else {
                        // More bytes to transfer: re-assert DRQ
                        beta->drq = true;
                        beta->drq_timer = 0;
                    }
                }
            }
            return beta->data_reg;
        }

        case 0xFF: {
            // System Register read — based on ESPectrum
            uint8_t val = 0;
            // Bit 7: INTRQ (either normal or forced)
            if (beta->intrq || beta->fintrq) val |= 0x80;
            // Bit 6: DRQ
            if (beta->drq) val |= 0x40;
            return val;
        }

        default:
            return 0xFF;
    }
}

void beta128_write(Beta128* beta, uint16_t port, uint8_t val) {
    switch (port & 0xFF) {
        case 0x1F:
            // Command Register
            exec_command(beta, val);
            break;

        case 0x3F:
            beta->track_reg = val;
            break;

        case 0x5F:
            beta->sector_reg = val;
            break;

        case 0x7F: {
            // Data Register — based on ESPectrum: clear DRQ on write
            beta->data_reg = val;
            beta->drq = false;

            if (beta->data_ptr && beta->data_write) {
                if (beta->data_pos < beta->data_len) {
                    beta->data_ptr[beta->data_pos++] = val;

                    if (beta->data_pos >= beta->data_len) {
                        if (beta->multi_sector && beta->cmd_type == WD_CMD_WRITE_SEC) {
                            beta->sector_reg++;
                            if (beta->sector_reg <= BETA_SECTORS_PER_TRACK) {
                                exec_write_sector(beta);
                            } else {
                                finish_command(beta, WD_ST_RNFERR);
                            }
                        } else {
                            finish_command(beta, 0);
                        }
                    } else {
                        // More bytes to accept: re-assert DRQ
                        beta->drq = true;
                        beta->drq_timer = 0;
                    }
                }
            }
            break;
        }

        case 0xFF: {
            // System Register write
            beta->system_reg = val;
            beta->sel_drive = val & 0x03;
            beta->sel_side = (val & 0x10) ? 0 : 1;  // bit4: 0=side1, 1=side0

            // Bit 2: FDC reset (active low)
            if (!(val & 0x04)) {
                beta128_reset(beta);
            }

            // Motor control: conectado al bit 3 (HLT)
            BetaDrive* drv = cur_drive(beta);
            drv->motor_on = true; // Motor siempre activo cuando Beta está paginado
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Carga de imágenes TRD
// ─────────────────────────────────────────────────────────────────────────────

bool beta128_load_trd(Beta128* beta, int drive, const char* path) {
    if (drive < 0 || drive >= BETA_MAX_DRIVES) return false;

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[BETA] No se pudo abrir TRD: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        return false;
    }

    BetaDrive* drv = &beta->drives[drive];

    // Liberar disco anterior si existía
    if (drv->disk.data) {
        free(drv->disk.data);
        drv->disk.data = NULL;
    }

    // Siempre asignar el tamaño completo del disco, rellenando con ceros
    drv->disk.data = (uint8_t*)calloc(1, BETA_DISK_SIZE);
    if (!drv->disk.data) {
        fclose(f);
        return false;
    }

    size_t to_read = (size_t)sz;
    if (to_read > BETA_DISK_SIZE) to_read = BETA_DISK_SIZE;
    if (fread(drv->disk.data, 1, to_read, f) != to_read) {
        fprintf(stderr, "[BETA] Lectura parcial TRD: %s\n", path);
    }
    fclose(f);

    drv->disk.data_size = BETA_DISK_SIZE;
    drv->disk.inserted = true;
    drv->disk.write_protected = false;

    // Detectar geometría a partir del tamaño del archivo
    if (sz <= 163840) {
        drv->disk.num_tracks = 40;
        drv->disk.num_sides = 1;
    } else if (sz <= 327680) {
        // Podría ser 40 tracks 2 sides o 80 tracks 1 side
        // La convención es 80 tracks 2 sides como default
        drv->disk.num_tracks = 80;
        drv->disk.num_sides = 2;
    } else {
        drv->disk.num_tracks = 80;
        drv->disk.num_sides = 2;
    }

    // Leer geometría del sector 9 de pista 0, side 0 (offset 0x8E3 = tipo de disco)
    if (sz >= 0x8E8) {
        uint8_t disk_type = drv->disk.data[0x8E3];
        switch (disk_type) {
            case 0x16: drv->disk.num_tracks = 80; drv->disk.num_sides = 2; break;
            case 0x17: drv->disk.num_tracks = 40; drv->disk.num_sides = 2; break;
            case 0x18: drv->disk.num_tracks = 80; drv->disk.num_sides = 1; break;
            case 0x19: drv->disk.num_tracks = 40; drv->disk.num_sides = 1; break;
            default: break; // Mantener la detección por tamaño
        }
    }

    printf("[BETA] TRD montado en drive %d: %s (%d pistas, %d caras)\n",
           drive, path, drv->disk.num_tracks, drv->disk.num_sides);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Carga de imágenes SCL
// ─────────────────────────────────────────────────────────────────────────────

bool beta128_load_scl(Beta128* beta, int drive, const char* path) {
    if (drive < 0 || drive >= BETA_MAX_DRIVES) return false;

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[BETA] No se pudo abrir SCL: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 9) {
        fclose(f);
        fprintf(stderr, "[BETA] SCL demasiado pequeño: %s\n", path);
        return false;
    }

    // Leer todo el archivo en memoria temporal
    uint8_t* scl_data = (uint8_t*)malloc((size_t)sz);
    if (!scl_data) { fclose(f); return false; }
    if (fread(scl_data, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "[BETA] Lectura parcial SCL: %s\n", path);
        free(scl_data);
        return false;
    }
    fclose(f);

    // Verificar firma "SINCLAIR"
    if (memcmp(scl_data, "SINCLAIR", 8) != 0) {
        fprintf(stderr, "[BETA] SCL: firma inválida en %s\n", path);
        free(scl_data);
        return false;
    }

    uint8_t num_files = scl_data[8];
    int header_size = 9 + num_files * 14;

    if (header_size > sz) {
        fprintf(stderr, "[BETA] SCL: archivo truncado %s\n", path);
        free(scl_data);
        return false;
    }

    BetaDrive* drv = &beta->drives[drive];
    if (drv->disk.data) {
        free(drv->disk.data);
        drv->disk.data = NULL;
    }

    // Crear disco vacío estándar (80 pistas, 2 caras)
    drv->disk.data = (uint8_t*)calloc(1, BETA_DISK_SIZE);
    if (!drv->disk.data) {
        free(scl_data);
        return false;
    }

    drv->disk.data_size = BETA_DISK_SIZE;
    drv->disk.inserted = true;
    drv->disk.write_protected = false;
    drv->disk.num_tracks = 80;
    drv->disk.num_sides = 2;

    // Escribir el catálogo TR-DOS y los datos de los ficheros
    // El catálogo ocupa los primeros 8 sectores (0x000-0x7FF) de la pista 0, side 0
    // Cada entrada del catálogo ocupa 16 bytes
    // El sector 9 (offset 0x800) contiene información del disco

    int data_offset = header_size; // posición en el SCL donde empiezan los datos
    int cur_track = 0;   // datos empiezan en logical track 1 (track 0, side 1)
    int cur_sec = 1;     // sector actual (1-16)
    int cur_side = 1;    // side 1 → logical track 1

    // Escribir entradas del catálogo
    for (int i = 0; i < num_files && i < 128; i++) {
        uint8_t* entry_src = &scl_data[9 + i * 14];
        uint8_t* entry_dst = &drv->disk.data[i * 16]; // catálogo en pista 0, side 0

        // Copiar 14 bytes del SCL entry → 16 bytes del catálogo TR-DOS:
        //   SCL [0..7]  → nombre, [8] tipo, [9..10] params, [11..12] longitud, [13] sectores
        //   TRD [0..7]  → nombre, [8] tipo, [9..10] params, [11..12] longitud,
        //       [13] sectores, [14] sector inicio (0-based), [15] pista lógica
        memcpy(entry_dst, entry_src, 8);   // nombre
        entry_dst[8]  = entry_src[8];      // tipo
        entry_dst[9]  = entry_src[9];      // param low
        entry_dst[10] = entry_src[10];     // param high
        entry_dst[11] = entry_src[11];     // longitud low
        entry_dst[12] = entry_src[12];     // longitud high

        uint8_t file_sectors = entry_src[13]; // longitud en sectores

        entry_dst[13] = file_sectors;
        entry_dst[14] = (uint8_t)(cur_sec - 1);                // sector inicio (0-based)
        entry_dst[15] = (uint8_t)(cur_track * 2 + cur_side);   // pista lógica

        // Copiar datos del fichero al disco
        for (int s = 0; s < file_sectors; s++) {
            int disk_off = disk_offset(cur_track, cur_side, cur_sec);
            if (disk_off >= 0 && disk_off + BETA_SECTOR_SIZE <= BETA_DISK_SIZE &&
                data_offset + BETA_SECTOR_SIZE <= sz) {
                memcpy(&drv->disk.data[disk_off], &scl_data[data_offset], BETA_SECTOR_SIZE);
            }
            data_offset += BETA_SECTOR_SIZE;

            // Avanzar al siguiente sector
            cur_sec++;
            if (cur_sec > BETA_SECTORS_PER_TRACK) {
                cur_sec = 1;
                cur_side++;
                if (cur_side >= 2) {
                    cur_side = 0;
                    cur_track++;
                }
            }
        }
    }

    // Sector 9 de pista 0, side 0 (offset 0x800): información del disco.
    // Layout según especificación TR-DOS (offsets absolutos en TRD):
    //   0x8E1: primer sector libre (0-based)
    //   0x8E2: primera pista libre (logical track)
    //   0x8E3: tipo de disco
    //   0x8E4: número de ficheros
    //   0x8E5-0x8E6: sectores libres (16-bit LE)
    //   0x8E7: ID TR-DOS (0x10)
    //   0x8F5-0x8FC: etiqueta del disco (8 bytes)
    drv->disk.data[0x8E1] = (uint8_t)(cur_sec - 1);               // primer sector libre
    drv->disk.data[0x8E2] = (uint8_t)(cur_track * 2 + cur_side);  // primera pista libre
    drv->disk.data[0x8E3] = 0x16;                                  // 80 pistas, 2 caras
    drv->disk.data[0x8E4] = num_files;

    int total_sectors = BETA_TRACKS_PER_SIDE * 2 * BETA_SECTORS_PER_TRACK;
    int used_sectors = ((cur_track * 2 + cur_side) * BETA_SECTORS_PER_TRACK) + (cur_sec - 1);
    int free_sectors = total_sectors - used_sectors;
    drv->disk.data[0x8E5] = (uint8_t)(free_sectors & 0xFF);
    drv->disk.data[0x8E6] = (uint8_t)((free_sectors >> 8) & 0xFF);

    drv->disk.data[0x8E7] = 0x10;  // TR-DOS ID

    memset(&drv->disk.data[0x8F5], ' ', 8);  // etiqueta del disco

    free(scl_data);

    printf("[BETA] SCL montado en drive %d: %s (%d ficheros)\n",
           drive, path, num_files);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Create empty formatted TRD disk (like ESPectrum default)
// ─────────────────────────────────────────────────────────────────────────────

bool beta128_insert_empty(Beta128* beta, int drive) {
    if (drive < 0 || drive >= BETA_MAX_DRIVES) return false;
    BetaDrive* drv = &beta->drives[drive];

    if (drv->disk.data) {
        free(drv->disk.data);
        drv->disk.data = NULL;
    }

    drv->disk.data = (uint8_t*)calloc(1, BETA_DISK_SIZE);
    if (!drv->disk.data) return false;
    drv->disk.data_size = BETA_DISK_SIZE;
    drv->disk.inserted = true;
    drv->disk.write_protected = false;
    drv->disk.num_tracks = 80;
    drv->disk.num_sides = 2;

    // Disk info sector (track 0, side 0, sector 9 → offset 0x800)
    drv->disk.data[0x8E1] = 0x00;  // first free sector (0-based)
    drv->disk.data[0x8E2] = 0x01;  // first free logical track (track 0 side 1)
    drv->disk.data[0x8E3] = 0x16;  // 80 tracks, double-sided
    drv->disk.data[0x8E4] = 0x00;  // 0 files
    // Free sectors: 2560 - 16 = 2544 = 0x09F0
    drv->disk.data[0x8E5] = 0xF0;  // free sectors low
    drv->disk.data[0x8E6] = 0x09;  // free sectors high
    drv->disk.data[0x8E7] = 0x10;  // TR-DOS ID byte
    memset(&drv->disk.data[0x8F5], ' ', 8);  // disk label

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expulsión de disco
// ─────────────────────────────────────────────────────────────────────────────

void beta128_eject(Beta128* beta, int drive) {
    if (drive < 0 || drive >= BETA_MAX_DRIVES) return;
    BetaDrive* drv = &beta->drives[drive];
    free(drv->disk.data);
    drv->disk.data = NULL;
    drv->disk.data_size = 0;
    drv->disk.inserted = false;
    drv->disk.write_protected = false;
    drv->disk.num_tracks = 0;
    drv->disk.num_sides = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// INTRQ y tick
// ─────────────────────────────────────────────────────────────────────────────

bool beta128_intrq(Beta128* beta) {
    return beta->intrq || beta->fintrq;
}

// Based on ESPectrum rvmWD1793Step() — simplified for instant-completion model
void beta128_tick(Beta128* beta, int delta) {
    // Index pulse simulation: 300 RPM = 200ms per revolution = ~70000 T-states at 3.5MHz
    // Index pulse duration: ~4ms = ~14000 T-states
    beta->index_counter += delta;
    if (beta->index_counter >= 70000) {
        beta->index_counter = 0;
        beta->index_pulse = true;
    } else if (beta->index_counter >= 14000) {
        beta->index_pulse = false;
    }

    // Command delay: count down before completing Type I commands or
    // asserting DRQ for READ ADDRESS.
    if (beta->cmd_delay > 0 && beta->busy) {
        beta->cmd_delay -= delta;
        if (beta->cmd_delay <= 0) {
            beta->cmd_delay = 0;
            if (beta->cmd_type >= WD_CMD_RESTORE && beta->cmd_type <= WD_CMD_STEP_OUT) {
                // Type I command completed — set head loaded and INTRQ
                beta->head_loaded = true;
                beta->busy = false;
                beta->intrq = true;
                beta->status = 0;  // Track0, WP, NOTREADY resolved dynamically
            } else {
                // Type II/III (READ ADDRESS): assert DRQ
                beta->drq = true;
                beta->drq_timer = 0;
            }
        }
    }

    // Lost data: if DRQ is asserted but host hasn't read/written,
    // auto-advance after timeout. READ_ADDR uses longer timeout (whole sector
    // pass time) since TR-DOS polling loops need DRQ to stay high longer.
    if (beta->drq && beta->busy) {
        beta->drq_timer += delta;
        int timeout = (beta->cmd_type == WD_CMD_READ_ADDR) ? 40000 : 192;
        if (beta->drq_timer >= timeout) {
            beta->drq_timer = 0;
            // Auto-advance: skip this byte (lost data)
            if (beta->data_ptr && beta->data_pos < beta->data_len) {
                beta->data_pos++;
                if (beta->data_pos >= beta->data_len) {
                    // Transfer complete (with lost data)
                    finish_command(beta, WD_ST_LOSTDATA);
                } else {
                    // More bytes — DRQ stays asserted, timer resets
                }
            } else {
                // No data buffer — finish immediately
                finish_command(beta, WD_ST_LOSTDATA);
            }
        }
    } else {
        beta->drq_timer = 0;
    }
}
