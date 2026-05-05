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
    beta->data_pos = 0;
    beta->data_len = 0;
    beta->data_ptr = NULL;
    beta->data_write = false;
    beta->multi_sector = false;
    beta->step_dir = 1;
    beta->sel_drive = 0;
    beta->sel_side = 0;
    beta->index_counter = 0;
    // No resetear discos montados
}

// ─────────────────────────────────────────────────────────────────────────────
// Ejecución de comandos WD1793
// ─────────────────────────────────────────────────────────────────────────────

static void finish_command(Beta128* beta, uint8_t status_bits) {
    beta->busy = false;
    beta->drq = false;
    beta->intrq = true;
    beta->status = status_bits;
    beta->cmd_type = WD_CMD_NONE;

    BetaDrive* drv = cur_drive(beta);
    if (!drv->disk.inserted) {
        beta->status |= WD_ST_NOTREADY;
    }
}

static void exec_restore(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);
    drv->current_track = 0;
    beta->track_reg = 0;
    beta->busy = false;
    beta->intrq = true;

    uint8_t st = WD_ST_TRACK0;
    if (drv->disk.inserted) {
        st |= WD_ST_HEADLOADED;
    } else {
        st |= WD_ST_NOTREADY;
    }
    beta->status = st;
}

static void exec_seek(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);
    int target = beta->data_reg;
    if (target < 0) target = 0;
    if (target >= BETA_TRACKS_PER_SIDE) target = BETA_TRACKS_PER_SIDE - 1;

    if (target > drv->current_track) beta->step_dir = 1;
    else if (target < drv->current_track) beta->step_dir = -1;

    drv->current_track = target;
    beta->track_reg = (uint8_t)target;
    beta->busy = false;
    beta->intrq = true;

    uint8_t st = WD_ST_HEADLOADED;
    if (drv->current_track == 0) st |= WD_ST_TRACK0;
    if (!drv->disk.inserted) st |= WD_ST_NOTREADY;
    beta->status = st;
}

static void exec_step(Beta128* beta, int direction) {
    BetaDrive* drv = cur_drive(beta);

    if (direction != 0) beta->step_dir = direction;
    int new_track = drv->current_track + beta->step_dir;
    if (new_track < 0) new_track = 0;
    if (new_track >= BETA_TRACKS_PER_SIDE) new_track = BETA_TRACKS_PER_SIDE - 1;

    drv->current_track = new_track;

    // Update track register si flag 'T' (bit 4) está activo
    if (beta->command_reg & 0x10) {
        beta->track_reg = (uint8_t)new_track;
    }

    beta->busy = false;
    beta->intrq = true;

    uint8_t st = WD_ST_HEADLOADED;
    if (drv->current_track == 0) st |= WD_ST_TRACK0;
    if (!drv->disk.inserted) st |= WD_ST_NOTREADY;
    beta->status = st;
}

static void exec_read_sector(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);

    if (!drv->disk.inserted) {
        finish_command(beta, WD_ST_NOTREADY);
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
    beta->busy = true;
    beta->status = WD_ST_BUSY | WD_ST_DRQ;
}

static void exec_write_sector(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);

    if (!drv->disk.inserted) {
        finish_command(beta, WD_ST_NOTREADY);
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
    beta->status = WD_ST_BUSY | WD_ST_DRQ;
}

static void exec_read_address(Beta128* beta) {
    BetaDrive* drv = cur_drive(beta);

    if (!drv->disk.inserted) {
        finish_command(beta, WD_ST_NOTREADY);
        return;
    }

    // Devuelve 6 bytes: track, side, sector, sector_size, crc1, crc2
    // Usamos un buffer estático en data_reg area
    static uint8_t id_buf[6];
    id_buf[0] = (uint8_t)drv->current_track;
    id_buf[1] = beta->sel_side;
    id_buf[2] = beta->sector_reg;
    id_buf[3] = 0x01; // size code 1 = 256 bytes
    id_buf[4] = 0x00; // CRC placeholder
    id_buf[5] = 0x00;

    beta->data_ptr = id_buf;
    beta->data_pos = 0;
    beta->data_len = 6;
    beta->data_write = false;
    beta->drq = true;
    beta->busy = true;
    beta->status = WD_ST_BUSY | WD_ST_DRQ;
}

static void exec_command(Beta128* beta, uint8_t cmd) {
    beta->command_reg = cmd;
    beta->intrq = false;

    // Type IV: Force Interrupt
    if ((cmd & 0xF0) == 0xD0) {
        beta->cmd_type = WD_CMD_FORCE_INT;
        beta->busy = false;
        beta->drq = false;
        beta->data_ptr = NULL;
        beta->data_pos = 0;
        beta->data_len = 0;
        // Interrumpir comando en curso, generar INTRQ si bits 0-3 != 0
        if (cmd & 0x0F) {
            beta->intrq = true;
        }
        // Reconstruir status como Type I
        BetaDrive* drv = cur_drive(beta);
        uint8_t st = 0;
        if (drv->disk.inserted) st |= WD_ST_HEADLOADED;
        else st |= WD_ST_NOTREADY;
        if (drv->current_track == 0) st |= WD_ST_TRACK0;
        beta->status = st;
        return;
    }

    // Type I: RESTORE, SEEK, STEP, STEP-IN, STEP-OUT
    if ((cmd & 0x80) == 0x00) {
        beta->busy = true;
        beta->status = WD_ST_BUSY;

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
        beta->cmd_type = WD_CMD_READ_ADDR;
        exec_read_address(beta);
        return;
    }

    if ((cmd & 0xF0) == 0xE0) {
        // READ TRACK - simplified: treat as reading the whole track
        beta->cmd_type = WD_CMD_READ_TRACK;
        BetaDrive* drv = cur_drive(beta);
        if (!drv->disk.inserted) {
            finish_command(beta, WD_ST_NOTREADY);
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
            beta->status = WD_ST_BUSY | WD_ST_DRQ;
        } else {
            finish_command(beta, WD_ST_RNFERR);
        }
        return;
    }

    if ((cmd & 0xF0) == 0xF0) {
        // WRITE TRACK (format) - simplified
        beta->cmd_type = WD_CMD_WRITE_TRACK;
        BetaDrive* drv = cur_drive(beta);
        if (!drv->disk.inserted) {
            finish_command(beta, WD_ST_NOTREADY);
            return;
        }
        if (drv->disk.write_protected) {
            finish_command(beta, WD_ST_WRPROTECT);
            return;
        }
        int off = disk_offset(drv->current_track, beta->sel_side, 1);
        if (off >= 0 && (size_t)(off + BETA_TRACK_SIZE) <= drv->disk.data_size) {
            beta->data_ptr = &drv->disk.data[off];
            beta->data_pos = 0;
            beta->data_len = BETA_TRACK_SIZE;
            beta->data_write = true;
            beta->drq = true;
            beta->busy = true;
            beta->status = WD_ST_BUSY | WD_ST_DRQ;
        } else {
            finish_command(beta, WD_ST_RNFERR);
        }
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lectura / Escritura de puertos
// ─────────────────────────────────────────────────────────────────────────────

uint8_t beta128_read(Beta128* beta, uint16_t port) {
    switch (port & 0xFF) {
        case 0x1F: {
            // Status Register
            uint8_t st = beta->status;
            beta->intrq = false;
            return st;
        }

        case 0x3F:
            return beta->track_reg;

        case 0x5F:
            return beta->sector_reg;

        case 0x7F: {
            // Data Register
            if (beta->drq && beta->data_ptr && !beta->data_write) {
                if (beta->data_pos < beta->data_len) {
                    beta->data_reg = beta->data_ptr[beta->data_pos++];

                    if (beta->data_pos >= beta->data_len) {
                        // Sector/data transfer completo
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
                    }
                }
            }
            return beta->data_reg;
        }

        case 0xFF: {
            // System Register read
            uint8_t val = 0;
            // Bit 7: INTRQ
            if (beta->intrq) val |= 0x80;
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
            // Data Register
            beta->data_reg = val;

            if (beta->drq && beta->data_ptr && beta->data_write) {
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

    // Leer la geometría del sector 8 de la pista 0 (info del disco)
    // Offset 0xE4 del disco = tipo de disco
    if (sz >= 0xE7) {
        uint8_t disk_type = drv->disk.data[0xE4];
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
    int cur_track = 1;   // pista actual para datos (pista 0 es catálogo)
    int cur_sec = 1;     // sector actual (1-16)
    int cur_side = 0;

    // Escribir entradas del catálogo
    for (int i = 0; i < num_files && i < 128; i++) {
        uint8_t* entry_src = &scl_data[9 + i * 14];
        uint8_t* entry_dst = &drv->disk.data[i * 16]; // catálogo en pista 0, side 0

        // Copiar 8 bytes de nombre + 1 byte tipo + 2 bytes parámetros
        memcpy(entry_dst, entry_src, 8);   // nombre
        entry_dst[8] = entry_src[8];       // tipo
        // Parámetros (bytes 9-11 del SCL entry)
        entry_dst[9]  = entry_src[9];
        entry_dst[10] = entry_src[10];
        entry_dst[11] = entry_src[11];

        uint8_t file_sectors = entry_src[13]; // longitud en sectores

        // Sector y pista de inicio
        entry_dst[13] = file_sectors;
        entry_dst[14] = (uint8_t)cur_sec;             // sector inicio (1-based, pero stored 0-based in some contexts)
        // Ajuste: en TR-DOS, el sector de inicio es 0-based (0..15)
        entry_dst[14] = (uint8_t)(cur_sec - 1);
        entry_dst[15] = (uint8_t)(cur_track * 2 + cur_side); // logical track

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

    // Escribir información del disco en sector 9 de la pista 0, side 0
    // Offset 0x800 en el TRD
    uint8_t* info = &drv->disk.data[0x800];
    info[0x00] = 0x00; // Primer sector libre (0-based)
    // El primer sector/track libre tras los datos escritos
    info[0x01] = (uint8_t)(cur_sec - 1);  // sector libre (0-based)
    info[0x02] = (uint8_t)(cur_track * 2 + cur_side); // logical track libre

    info[0x04] = num_files;
    // Total de sectores libres
    int total_sectors = BETA_TRACKS_PER_SIDE * 2 * BETA_SECTORS_PER_TRACK;
    int used_sectors = ((cur_track * 2 + cur_side) * BETA_SECTORS_PER_TRACK) + (cur_sec - 1);
    int free_sectors = total_sectors - used_sectors;
    info[0x05] = (uint8_t)(free_sectors & 0xFF);
    info[0x06] = (uint8_t)((free_sectors >> 8) & 0xFF);

    info[0x07] = 0x10; // TR-DOS identificador

    // Bytes 0x08-0x0F: no usados
    info[0x0D] = 0x00; // no usados

    // Label del disco (bytes E0-E7 = offset 0x8E0)
    uint8_t* label = &drv->disk.data[0x8E0];
    memset(label, ' ', 8);

    // Tipo de disco
    drv->disk.data[0x8E4] = 0x16; // 80 pistas, 2 caras

    free(scl_data);

    printf("[BETA] SCL montado en drive %d: %s (%d ficheros)\n",
           drive, path, num_files);
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
    return beta->intrq;
}

void beta128_tick(Beta128* beta, int delta) {
    (void)delta;
    // Simular index pulse: cada ~200ms (aprox 10 revoluciones/segundo)
    // El index pulse dura muy poco, aquí simplemente lo mantenemos
    // disponible cuando se lee el status y no hay comando activo.
    beta->index_counter += delta;
    if (beta->index_counter >= 70000) { // ~1 revolución
        beta->index_counter = 0;
        // Index pulse visible en status solo durante Type I idle
        if (!beta->busy) {
            beta->status |= WD_ST_INDEX;
        }
    } else if (beta->index_counter >= 1000) {
        // Quitar index pulse después de un breve período
        if (!beta->busy) {
            beta->status &= ~WD_ST_INDEX;
        }
    }
}
