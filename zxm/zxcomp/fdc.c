/*
 * fdc.c - NEC uPD765 Floppy Disk Controller + DSK disk image support
 *
 * Supports:
 *   - Standard CPCEMU DSK format
 *   - Extended DSK format (variable sector sizes)
 *   - Read Data, Write Data, Read ID, Seek, Recalibrate,
 *     Sense Interrupt, Specify, Format Track
 */

#include "fdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── DSK loader ─────────────────────────────────────────────────────────── */

/* Standard DSK header */
#define DSK_HEADER_STD "MV - CPCEMU Disk-File\r\nDisk-Info\r\n"
#define DSK_HEADER_EXT "EXTENDED CPC DSK File\r\nDisk-Info\r\n"
#define DSK_TRACK_HEADER "Track-Info\r\n"

static int dsk_load(dsk_disk_t *disk, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr,"FDC: cannot open %s\n", path); return -1; }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    disk->data_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    disk->data = (uint8_t*)malloc(disk->data_size);
    if (!disk->data) { fclose(f); return -1; }
    fread(disk->data, 1, disk->data_size, f);
    fclose(f);

    /* Detect format */
    if (memcmp(disk->data, DSK_HEADER_EXT, 8) == 0) {
        disk->extended = 1;
    } else if (memcmp(disk->data, DSK_HEADER_STD, 8) == 0) {
        disk->extended = 0;
    } else {
        fprintf(stderr,"FDC: unknown DSK format\n");
        free(disk->data); disk->data = NULL;
        return -1;
    }

    disk->track_count = disk->data[0x30];
    disk->side_count  = disk->data[0x31];

    printf("FDC: DSK %s, %d tracks, %d sides, %s format\n",
           path, disk->track_count, disk->side_count,
           disk->extended ? "Extended" : "Standard");

    /* Parse track table */
    uint32_t offset = 0x100; /* first track starts at 0x100 */

    for (int t = 0; t < disk->track_count; t++) {
        for (int s = 0; s < disk->side_count; s++) {
            int idx = t * disk->side_count + s;

            if (!disk->extended) {
                /* Standard: all tracks same size */
                uint16_t track_size = disk->data[0x32] | (disk->data[0x33] << 8);
                /* Parse track info block at `offset` */
                uint8_t *tb = disk->data + offset;
                if (offset + 0x18 > disk->data_size) break;
                if (memcmp(tb, "Track-Info", 10) != 0) { offset += track_size; continue; }

                disk->tracks[idx].sector_count = tb[0x15];
                uint32_t sec_data_off = offset + 0x100;

                for (int sec = 0; sec < disk->tracks[idx].sector_count; sec++) {
                    uint8_t *si = tb + 0x18 + sec * 8;
                    dsk_sector_t *ds = &disk->tracks[idx].sectors[sec];
                    ds->track     = si[0];
                    ds->side      = si[1];
                    ds->sector_id = si[2];
                    ds->size_code = si[3];
                    ds->fdcst1    = si[4];
                    ds->fdcst2    = si[5];
                    ds->data_len  = 128 << ds->size_code;
                    ds->offset    = sec_data_off;
                    sec_data_off += ds->data_len;
                }
                offset += track_size;
            } else {
                /* Extended: track sizes in table at 0x34 */
                uint16_t track_size = disk->data[0x34 + idx] * 256;
                if (track_size == 0) { continue; } /* unformatted */
                uint8_t *tb = disk->data + offset;
                if (offset + 0x18 > disk->data_size) break;
                if (memcmp(tb, "Track-Info", 10) != 0) { offset += track_size; continue; }

                disk->tracks[idx].sector_count = tb[0x15];
                uint32_t sec_data_off = offset + 0x100;

                for (int sec = 0; sec < disk->tracks[idx].sector_count; sec++) {
                    uint8_t *si = tb + 0x18 + sec * 8;
                    dsk_sector_t *ds = &disk->tracks[idx].sectors[sec];
                    ds->track     = si[0];
                    ds->side      = si[1];
                    ds->sector_id = si[2];
                    ds->size_code = si[3];
                    ds->fdcst1    = si[4];
                    ds->fdcst2    = si[5];
                    /* Extended DSK: actual size in bytes 6:7 of SIT */
                    ds->data_len  = si[6] | (si[7] << 8);
                    if (ds->data_len == 0) ds->data_len = 128 << ds->size_code;
                    ds->offset    = sec_data_off;
                    sec_data_off += ds->data_len;
                }
                offset += track_size;
            }
        }
    }

    disk->loaded = 1;
    return 0;
}

static void dsk_unload(dsk_disk_t *disk)
{
    if (disk->data) { free(disk->data); disk->data = NULL; }
    memset(disk, 0, sizeof(*disk));
}

/* ── FDC init ───────────────────────────────────────────────────────────── */

int fdc_init(fdc_t *fdc)
{
    memset(fdc, 0, sizeof(*fdc));
    fdc->state       = FDC_STATE_IDLE;
    fdc->main_status = 0x80; /* RQM=1, DIO=0, CB=0 */
    return 0;
}

void fdc_destroy(fdc_t *fdc)
{
    for (int i = 0; i < FDC_MAX_DRIVES; i++)
        dsk_unload(&fdc->drive[i].disk);
    if (fdc->dma_buf) { free(fdc->dma_buf); fdc->dma_buf = NULL; }
}

int fdc_insert_disk(fdc_t *fdc, const char *path, int drive)
{
    if (drive < 0 || drive >= FDC_MAX_DRIVES) return -1;
    dsk_unload(&fdc->drive[drive].disk);
    return dsk_load(&fdc->drive[drive].disk, path);
}

void fdc_eject_disk(fdc_t *fdc, int drive)
{
    if (drive < 0 || drive >= FDC_MAX_DRIVES) return;
    dsk_unload(&fdc->drive[drive].disk);
}

/* ── FDC internal helpers ─────────────────────────────────────────────── */

static void fdc_set_result(fdc_t *fdc, uint8_t *res, int len)
{
    memcpy(fdc->result, res, len);
    fdc->result_len  = len;
    fdc->result_pos  = 0;
    fdc->state       = FDC_STATE_RESULT;
    fdc->main_status = 0xD0; /* RQM=1, DIO=1, CB=1 */
}

static dsk_sector_t *fdc_find_sector(fdc_t *fdc, int drive, int track, int side,
                                      int sector_id)
{
    dsk_disk_t *disk = &fdc->drive[drive].disk;
    if (!disk->loaded) return NULL;
    if (track >= disk->track_count) return NULL;

    int idx = track * disk->side_count + (side % disk->side_count);
    dsk_track_t *tr = &disk->tracks[idx];

    for (int s = 0; s < tr->sector_count; s++) {
        if (tr->sectors[s].sector_id == sector_id)
            return &tr->sectors[s];
    }
    return NULL;
}

/* ── Execute a command ──────────────────────────────────────────────────── */

static void fdc_exec_command(fdc_t *fdc)
{
    uint8_t cmd_code = fdc->cmd[0] & 0x1F;
    int drive = fdc->cmd[1] & 1;
    int side  = (fdc->cmd[1] >> 2) & 1;
    int track = fdc->cmd[2];

    fdc->current_drive = drive;
    fdc->current_side  = side;

    switch (cmd_code) {

    /* ── SPECIFY ──────────────────────────────────────────────── */
    case FDC_CMD_SPECIFY:
        /* Just absorb parameters, no result */
        fdc->state       = FDC_STATE_IDLE;
        fdc->main_status = 0x80;
        break;

    /* ── SENSE INTERRUPT ──────────────────────────────────────── */
    case FDC_CMD_SENSE_INT: {
        uint8_t res[2] = { fdc->st0, fdc->drive[drive].current_track };
        fdc_set_result(fdc, res, 2);
        fdc->st0 = 0;
        break;
    }

    /* ── SENSE DRIVE STATUS ───────────────────────────────────── */
    case FDC_CMD_SENSE_DRIVE: {
        uint8_t st3 = drive & 1;
        if (side) st3 |= 0x04;
        st3 |= 0x20; /* ready */
        if (fdc->drive[drive].current_track == 0) st3 |= 0x10;
        if (fdc->drive[drive].write_protect)       st3 |= 0x40;
        uint8_t res[1] = { st3 };
        fdc_set_result(fdc, res, 1);
        break;
    }

    /* ── RECALIBRATE ──────────────────────────────────────────── */
    case FDC_CMD_RECALIBRATE:
        fdc->drive[drive].current_track = 0;
        fdc->st0 = 0x20 | drive; /* seek end */
        fdc->state       = FDC_STATE_IDLE;
        fdc->main_status = 0x80;
        break;

    /* ── SEEK ─────────────────────────────────────────────────── */
    case FDC_CMD_SEEK:
        fdc->drive[drive].current_track = fdc->cmd[2];
        fdc->st0 = 0x20 | drive;
        fdc->state       = FDC_STATE_IDLE;
        fdc->main_status = 0x80;
        break;

    /* ── READ ID ──────────────────────────────────────────────── */
    case FDC_CMD_READ_ID: {
        dsk_disk_t *disk = &fdc->drive[drive].disk;
        int cur_track = fdc->drive[drive].current_track;
        if (!disk->loaded || cur_track >= disk->track_count) {
            /* No disk / bad track */
            uint8_t res[7] = { 0x40|drive, 0x04, 0x00, cur_track, 0, 1, 2 };
            fdc_set_result(fdc, res, 7);
        } else {
            int idx = cur_track * disk->side_count + (side % disk->side_count);
            dsk_track_t *tr = &disk->tracks[idx];
            if (tr->sector_count == 0) {
                uint8_t res[7] = { 0x40|drive, 0x04, 0x00, cur_track, 0, 1, 2 };
                fdc_set_result(fdc, res, 7);
            } else {
                /* Return first sector ID */
                dsk_sector_t *sec = &tr->sectors[0];
                uint8_t res[7] = { drive, 0x00, 0x00,
                    sec->track, sec->side, sec->sector_id, sec->size_code };
                fdc_set_result(fdc, res, 7);
            }
        }
        break;
    }

    /* ── READ DATA ────────────────────────────────────────────── */
    case FDC_CMD_READ_DATA:
    case FDC_CMD_READ_DELETED: {
        int sector_id  = fdc->cmd[4];
        int size_code  = fdc->cmd[5];
        dsk_sector_t *sec = fdc_find_sector(fdc, drive, track, side, sector_id);

        if (!sec) {
            /* Sector not found */
            uint8_t res[7] = { 0x40|drive, 0x04, 0x00, track, side, sector_id, size_code };
            fdc_set_result(fdc, res, 7);
            break;
        }

        /* Setup DMA read buffer */
        if (fdc->dma_buf) free(fdc->dma_buf);
        fdc->dma_len  = sec->data_len;
        fdc->dma_buf  = (uint8_t*)malloc(fdc->dma_len);
        fdc->dma_pos  = 0;
        fdc->dma_read = 1;

        dsk_disk_t *disk = &fdc->drive[drive].disk;
        if (sec->offset + sec->data_len <= disk->data_size) {
            memcpy(fdc->dma_buf, disk->data + sec->offset, sec->data_len);
        } else {
            memset(fdc->dma_buf, 0xE5, sec->data_len);
        }

        fdc->state = FDC_STATE_EXEC;
        fdc->main_status = 0xF0; /* RQM=1, DIO=1, EXM=1, CB=1 */

        /* Save result for after transfer */
        fdc->result[0] = drive;
        fdc->result[1] = 0x00; fdc->result[2] = 0x00;
        fdc->result[3] = track; fdc->result[4] = side;
        fdc->result[5] = sector_id; fdc->result[6] = size_code;
        fdc->result_len = 7;
        break;
    }

    /* ── WRITE DATA ───────────────────────────────────────────── */
    case FDC_CMD_WRITE_DATA:
    case FDC_CMD_WRITE_DELETED: {
        int sector_id = fdc->cmd[4];
        int size_code = fdc->cmd[5];
        dsk_sector_t *sec = fdc_find_sector(fdc, drive, track, side, sector_id);

        if (!sec || fdc->drive[drive].write_protect) {
            uint8_t st1 = fdc->drive[drive].write_protect ? 0x02 : 0x04;
            uint8_t res[7] = { 0x40|drive, st1, 0x00, track, side, sector_id, size_code };
            fdc_set_result(fdc, res, 7);
            break;
        }

        if (fdc->dma_buf) free(fdc->dma_buf);
        fdc->dma_len  = sec->data_len;
        fdc->dma_buf  = (uint8_t*)calloc(fdc->dma_len, 1);
        fdc->dma_pos  = 0;
        fdc->dma_read = 0;

        /* Store sector reference for write-back */
        fdc->current_track  = track;
        fdc->current_sector = sector_id;

        fdc->state = FDC_STATE_EXEC;
        fdc->main_status = 0xB0; /* RQM=1, DIO=0, EXM=1, CB=1 */

        fdc->result[0] = drive;
        fdc->result[1] = 0x00; fdc->result[2] = 0x00;
        fdc->result[3] = track; fdc->result[4] = side;
        fdc->result[5] = sector_id; fdc->result[6] = size_code;
        fdc->result_len = 7;
        break;
    }

    /* ── FORMAT TRACK ─────────────────────────────────────────── */
    case FDC_CMD_FORMAT_TRACK: {
        /* Just ACK - complex formatting not fully emulated */
        uint8_t res[7] = { drive, 0x00, 0x00, track, side, 1, fdc->cmd[2] };
        fdc_set_result(fdc, res, 7);
        break;
    }

    default:
        /* Invalid command */
        fdc->result[0] = 0x80;
        fdc->result_len = 1;
        fdc->result_pos = 0;
        fdc->state = FDC_STATE_RESULT;
        fdc->main_status = 0xD0;
        break;
    }
}

/* ── FDC register read/write ────────────────────────────────────────────── */

/* reg=0: Main Status Register
 * reg=1: Data register */
uint8_t fdc_read(fdc_t *fdc, int reg)
{
    if (reg == 0) {
        return fdc->main_status;
    }

    /* Data register */
    switch (fdc->state) {
    case FDC_STATE_EXEC:
        if (fdc->dma_read) {
            /* CPU reading sector data */
            if (fdc->dma_pos < fdc->dma_len) {
                uint8_t b = fdc->dma_buf[fdc->dma_pos++];
                if (fdc->dma_pos >= fdc->dma_len) {
                    /* Transfer complete -> result phase */
                    fdc_set_result(fdc, fdc->result, fdc->result_len);
                }
                return b;
            }
        }
        return 0xFF;

    case FDC_STATE_RESULT:
        if (fdc->result_pos < fdc->result_len) {
            uint8_t b = fdc->result[fdc->result_pos++];
            if (fdc->result_pos >= fdc->result_len) {
                fdc->state       = FDC_STATE_IDLE;
                fdc->main_status = 0x80;
            }
            return b;
        }
        return 0xFF;

    default:
        return 0xFF;
    }
}

void fdc_write(fdc_t *fdc, int reg, uint8_t val)
{
    if (reg == 0) {
        /* Control register: motor on/off */
        for (int i = 0; i < FDC_MAX_DRIVES; i++)
            fdc->drive[i].motor_on = (val >> i) & 1;
        return;
    }

    /* Data register */
    switch (fdc->state) {
    case FDC_STATE_IDLE:
        /* Start new command */
        fdc->cmd[0]      = val;
        fdc->cmd_pos     = 1;
        fdc->state       = FDC_STATE_COMMAND;
        fdc->main_status = 0x90; /* RQM=1, CB=1 */

        /* Determine expected command length */
        switch (val & 0x1F) {
        case FDC_CMD_SPECIFY:      fdc->cmd_len = 3; break;
        case FDC_CMD_SENSE_DRIVE:  fdc->cmd_len = 2; break;
        case FDC_CMD_RECALIBRATE:  fdc->cmd_len = 2; break;
        case FDC_CMD_SENSE_INT:
            fdc->cmd_len = 1;
            fdc->cmd_pos = 1;
            fdc_exec_command(fdc);
            break;
        case FDC_CMD_SEEK:         fdc->cmd_len = 3; break;
        case FDC_CMD_READ_ID:      fdc->cmd_len = 2; break;
        case FDC_CMD_READ_DATA:
        case FDC_CMD_READ_DELETED:
        case FDC_CMD_WRITE_DATA:
        case FDC_CMD_WRITE_DELETED:fdc->cmd_len = 9; break;
        case FDC_CMD_FORMAT_TRACK: fdc->cmd_len = 6; break;
        case FDC_CMD_READ_TRACK:   fdc->cmd_len = 9; break;
        default:                   fdc->cmd_len = 1;
            fdc->state = FDC_STATE_IDLE;
            fdc->main_status = 0x80;
            break;
        }
        break;

    case FDC_STATE_COMMAND:
        if (fdc->cmd_pos < 10) {
            fdc->cmd[fdc->cmd_pos++] = val;
        }
        if (fdc->cmd_pos >= fdc->cmd_len) {
            fdc_exec_command(fdc);
        }
        break;

    case FDC_STATE_EXEC:
        if (!fdc->dma_read) {
            /* CPU writing sector data */
            if (fdc->dma_pos < fdc->dma_len) {
                fdc->dma_buf[fdc->dma_pos++] = val;
                if (fdc->dma_pos >= fdc->dma_len) {
                    /* Write sector data back to disk image */
                    int drive = fdc->current_drive;
                    int side  = fdc->current_side;
                    int track = fdc->current_track;
                    dsk_sector_t *sec = fdc_find_sector(fdc, drive, track, side,
                                                         fdc->current_sector);
                    if (sec) {
                        dsk_disk_t *disk = &fdc->drive[drive].disk;
                        if (sec->offset + sec->data_len <= disk->data_size) {
                            memcpy(disk->data + sec->offset,
                                   fdc->dma_buf, sec->data_len);
                        }
                    }
                    fdc_set_result(fdc, fdc->result, fdc->result_len);
                }
            }
        }
        break;

    default:
        break;
    }
}

/* ── FDC tick (called at ~1MHz) ────────────────────────────────────────── */
void fdc_tick(fdc_t *fdc)
{
    /* Delay counter for seek emulation */
    if (fdc->delay > 0) fdc->delay--;
}
