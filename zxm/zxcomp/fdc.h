/*
 * fdc.h - NEC uPD765 / Intel 8272 Floppy Disk Controller
 *
 * The CPC6128 uses an FDC for disk access.
 * Supports standard DSK format (CPCEMU/Extended).
 */

#ifndef FDC_H
#define FDC_H

#include <stdint.h>
#include <stdio.h>

#define FDC_MAX_DRIVES    2
#define FDC_MAX_TRACKS    84
#define FDC_MAX_SECTORS   18
#define FDC_SECTOR_SIZE   512
#define FDC_MAX_SECTOR_SIZE 8192

/* FDC state machine */
typedef enum {
    FDC_STATE_IDLE,
    FDC_STATE_COMMAND,
    FDC_STATE_EXEC,
    FDC_STATE_RESULT
} fdc_state_t;

/* FDC commands */
#define FDC_CMD_READ_DATA        0x06
#define FDC_CMD_WRITE_DATA       0x05
#define FDC_CMD_READ_ID          0x0A
#define FDC_CMD_SPECIFY          0x03
#define FDC_CMD_SENSE_INT        0x08
#define FDC_CMD_SENSE_DRIVE      0x04
#define FDC_CMD_SEEK             0x0F
#define FDC_CMD_RECALIBRATE      0x07
#define FDC_CMD_READ_DELETED     0x0C
#define FDC_CMD_WRITE_DELETED    0x09
#define FDC_CMD_FORMAT_TRACK     0x0D
#define FDC_CMD_READ_TRACK       0x02
#define FDC_CMD_SCAN_EQUAL       0x11
#define FDC_CMD_SCAN_LOW_EQUAL   0x19
#define FDC_CMD_SCAN_HIGH_EQUAL  0x1D

/* DSK sector info */
typedef struct {
    uint8_t  track, side, sector_id, size_code;
    uint8_t  fdcst1, fdcst2;
    uint16_t data_len;      /* actual byte count in disk image */
    uint32_t offset;        /* offset in DSK image file */
} dsk_sector_t;

/* DSK track info */
typedef struct {
    int          sector_count;
    dsk_sector_t sectors[FDC_MAX_SECTORS];
} dsk_track_t;

/* DSK disk */
typedef struct {
    int         loaded;
    int         track_count;
    int         side_count;
    int         extended;        /* 1=extended DSK format */
    dsk_track_t tracks[FDC_MAX_TRACKS * 2]; /* [track*2+side] */
    uint8_t    *data;            /* raw disk data */
    size_t      data_size;
} dsk_disk_t;

/* Drive state */
typedef struct {
    dsk_disk_t  disk;
    int         current_track;
    int         motor_on;
    int         write_protect;
} fdc_drive_t;

typedef struct fdc_s {
    /* 8272 state */
    fdc_state_t state;
    uint8_t     cmd[10];
    int         cmd_len;
    int         cmd_pos;
    uint8_t     result[10];
    int         result_len;
    int         result_pos;

    /* Status */
    uint8_t     main_status;     /* MSR */
    uint8_t     st0, st1, st2;

    /* DMA / data transfer */
    uint8_t    *dma_buf;
    int         dma_len;
    int         dma_pos;
    int         dma_read;        /* 1=cpu reads, 0=cpu writes */

    /* Drives */
    fdc_drive_t drive[FDC_MAX_DRIVES];
    int         current_drive;
    int         current_side;
    int         current_track;
    int         current_sector;

    /* Timing */
    int         delay;
} fdc_t;

int     fdc_init        (fdc_t *fdc);
void    fdc_destroy     (fdc_t *fdc);
int     fdc_insert_disk (fdc_t *fdc, const char *path, int drive);
void    fdc_eject_disk  (fdc_t *fdc, int drive);
uint8_t fdc_read        (fdc_t *fdc, int reg);
void    fdc_write       (fdc_t *fdc, int reg, uint8_t val);
void    fdc_tick        (fdc_t *fdc);

#endif /* FDC_H */
