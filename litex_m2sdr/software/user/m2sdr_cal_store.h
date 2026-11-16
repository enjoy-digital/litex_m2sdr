/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR reference clock calibration: stored record.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_CAL_STORE_H
#define M2SDR_CAL_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the record, in a file or in the flash. */
#define M2SDR_CAL_RECORD_BYTES 128
#define M2SDR_CAL_NOTE_BYTES   64

/* Default place of the record in the SPI flash: the last 64 KiB of the operational slot
 * (8 MiB at 0x00800000). The bitstreams are compressed and end far below it, and a gateware
 * update only erases the blocks that its image covers. */
#define M2SDR_CAL_FLASH_OFFSET 0x00ff0000u

struct m2sdr_cal_record {
    double   refclk_ppm;                 /* Reference clock error (positive: fast). */
    double   spread_ppm;                 /* Spread of the measurements behind it. */
    double   temperature_c;              /* FPGA temperature at the measurement. */
    uint64_t unix_time;                  /* Date of the measurement, seconds. */
    uint64_t fpga_dna;                   /* Board the record belongs to. */
    uint32_t signals;                    /* Number of signals measured. */
    char     note[M2SDR_CAL_NOTE_BYTES]; /* Free text: the signals used. */
};

/* Record to and from its M2SDR_CAL_RECORD_BYTES bytes (little-endian, CRC-32 at the end).
 * The decode returns 0, or -1 for anything else than a valid record: erased flash included. */
void m2sdr_cal_record_encode(const struct m2sdr_cal_record *rec, uint8_t *buf);
int  m2sdr_cal_record_decode(const uint8_t *buf, struct m2sdr_cal_record *rec);

/* Record in a file. Return 0, or -1 with errno set (ENOENT: no record yet, EINVAL: not a
 * valid record). */
int m2sdr_cal_file_save(const char *path, const struct m2sdr_cal_record *rec);
int m2sdr_cal_file_load(const char *path, struct m2sdr_cal_record *rec);

/* Default file of a board: $XDG_CONFIG_HOME/litex_m2sdr/cal_<dna>.bin, or under ~/.config. */
int m2sdr_cal_default_path(uint64_t fpga_dna, char *path, size_t len);

/* Device side (m2sdr_cal_dev.c) */
/*------------------------------*/

struct m2sdr_dev;

enum m2sdr_cal_source {
    M2SDR_CAL_SOURCE_NONE  = 0,
    M2SDR_CAL_SOURCE_FLASH = 1,
    M2SDR_CAL_SOURCE_FILE  = 2,
};

/* Record in the SPI flash of the board, at flash_offset (M2SDR_CAL_FLASH_OFFSET by default).
 * The save erases and writes the erase block of the offset, and reads it back. Return 0, or
 * -1 (load: no valid record there). */
int m2sdr_cal_flash_load(struct m2sdr_dev *dev, uint32_t flash_offset, struct m2sdr_cal_record *rec);
int m2sdr_cal_flash_save(struct m2sdr_dev *dev, uint32_t flash_offset, const struct m2sdr_cal_record *rec);
int m2sdr_cal_flash_erase(struct m2sdr_dev *dev, uint32_t flash_offset);

/* Record of an open board: the flash first, then the default file of the board. A record of
 * another board (FPGA DNA) is refused. Returns the source, M2SDR_CAL_SOURCE_NONE without one. */
enum m2sdr_cal_source m2sdr_cal_load(struct m2sdr_dev *dev, uint32_t flash_offset,
                                     struct m2sdr_cal_record *rec);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_CAL_STORE_H */
