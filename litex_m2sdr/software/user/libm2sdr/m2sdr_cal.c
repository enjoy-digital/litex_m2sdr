/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR reference clock calibration: record in a file or in the SPI flash of a board.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "m2sdr.h"
#include "m2sdr_cal.h"
#include "m2sdr_flash.h"

#define CAL_MAGIC   "M2SDRCAL"
#define CAL_VERSION 1u

/* Layout: magic (8), version (4), signals (4), refclk_ppm (8), spread_ppm (8),
 * temperature_c (8), unix_time (8), fpga_dna (8), note (64), reserved (4), crc32 (4). */

static uint32_t crc32_ieee(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_le(uint8_t *buf, uint64_t v, int bytes)
{
    int i;
    for (i = 0; i < bytes; i++)
        buf[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *buf, int bytes)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < bytes; i++)
        v |= (uint64_t)buf[i] << (8 * i);
    return v;
}

static void put_double(uint8_t *buf, double d)
{
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    put_le(buf, v, 8);
}

static double get_double(const uint8_t *buf)
{
    const uint64_t v = get_le(buf, 8);
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

void m2sdr_cal_record_encode(const struct m2sdr_cal_record *rec, uint8_t *buf)
{
    memset(buf, 0, M2SDR_CAL_RECORD_BYTES);
    memcpy(buf, CAL_MAGIC, 8);
    put_le(buf + 8, CAL_VERSION, 4);
    put_le(buf + 12, rec->signals, 4);
    put_double(buf + 16, rec->refclk_ppm);
    put_double(buf + 24, rec->spread_ppm);
    put_double(buf + 32, rec->temperature_c);
    put_le(buf + 40, rec->unix_time, 8);
    put_le(buf + 48, rec->fpga_dna, 8);
    memcpy(buf + 56, rec->note, M2SDR_CAL_NOTE_BYTES - 1);
    put_le(buf + 124, crc32_ieee(buf, 124), 4);
}

int m2sdr_cal_record_decode(const uint8_t *buf, struct m2sdr_cal_record *rec)
{
    if (memcmp(buf, CAL_MAGIC, 8) != 0)
        return -1;
    if (get_le(buf + 8, 4) != CAL_VERSION)
        return -1;
    if ((uint32_t)get_le(buf + 124, 4) != crc32_ieee(buf, 124))
        return -1;

    memset(rec, 0, sizeof(*rec));
    rec->signals       = (uint32_t)get_le(buf + 12, 4);
    rec->refclk_ppm    = get_double(buf + 16);
    rec->spread_ppm    = get_double(buf + 24);
    rec->temperature_c = get_double(buf + 32);
    rec->unix_time     = get_le(buf + 40, 8);
    rec->fpga_dna      = get_le(buf + 48, 8);
    memcpy(rec->note, buf + 56, M2SDR_CAL_NOTE_BYTES - 1);
    return 0;
}

int m2sdr_cal_default_path(uint64_t fpga_dna, char *path, size_t len)
{
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;

    if (xdg && xdg[0])
        n = snprintf(path, len, "%s/litex_m2sdr/cal_%014llx.bin", xdg, (unsigned long long)fpga_dna);
    else if (home && home[0])
        n = snprintf(path, len, "%s/.config/litex_m2sdr/cal_%014llx.bin", home, (unsigned long long)fpga_dna);
    else
        return -1;
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

/* Create the directories of a file path. */
static void make_parents(const char *path)
{
    char tmp[1024];
    char *p;

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

int m2sdr_cal_file_save(const char *path, const struct m2sdr_cal_record *rec)
{
    uint8_t buf[M2SDR_CAL_RECORD_BYTES];
    FILE *f;
    int ok;

    m2sdr_cal_record_encode(rec, buf);
    make_parents(path);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    ok = fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);
    if (fclose(f) != 0)
        ok = 0;
    if (!ok) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int m2sdr_cal_file_load(const char *path, struct m2sdr_cal_record *rec)
{
    uint8_t buf[M2SDR_CAL_RECORD_BYTES];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n != sizeof(buf) || m2sdr_cal_record_decode(buf, rec) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* Record of a board */
/*-------------------*/

int m2sdr_cal_flash_load(struct m2sdr_dev *dev, uint32_t flash_offset, struct m2sdr_cal_record *rec)
{
    uint8_t buf[M2SDR_CAL_RECORD_BYTES];
    void *handle = m2sdr_get_handle(dev);

    if (!handle || m2sdr_flash_read_range(handle, flash_offset, buf, sizeof(buf)) != 0)
        return -1;
    return m2sdr_cal_record_decode(buf, rec);
}

/* Erase the block of the record and write `buf` (or nothing) at its start. */
static int flash_write_block(struct m2sdr_dev *dev, uint32_t flash_offset, const uint8_t *buf)
{
    void *handle = m2sdr_get_handle(dev);
    int block;
    uint8_t *data;
    int errors;

    if (!handle)
        return -1;
    block = m2sdr_flash_get_erase_block_size(handle);
    if (block < M2SDR_CAL_RECORD_BYTES || (flash_offset % (uint32_t)block) != 0)
        return -1;
    data = malloc((size_t)block);
    if (!data)
        return -1;
    memset(data, 0xff, (size_t)block);
    if (buf)
        memcpy(data, buf, M2SDR_CAL_RECORD_BYTES);
    errors = m2sdr_flash_write(handle, data, flash_offset, (uint32_t)block, NULL, NULL);
    free(data);
    return errors ? -1 : 0;
}

int m2sdr_cal_flash_save(struct m2sdr_dev *dev, uint32_t flash_offset, const struct m2sdr_cal_record *rec)
{
    uint8_t buf[M2SDR_CAL_RECORD_BYTES];
    struct m2sdr_cal_record back;

    m2sdr_cal_record_encode(rec, buf);
    if (flash_write_block(dev, flash_offset, buf) != 0)
        return -1;
    if (m2sdr_cal_flash_load(dev, flash_offset, &back) != 0 || back.refclk_ppm != rec->refclk_ppm)
        return -1;
    return 0;
}

int m2sdr_cal_flash_erase(struct m2sdr_dev *dev, uint32_t flash_offset)
{
    return flash_write_block(dev, flash_offset, NULL);
}

enum m2sdr_cal_source m2sdr_cal_load(struct m2sdr_dev *dev, uint32_t flash_offset,
                                     struct m2sdr_cal_record *rec)
{
    uint64_t dna = 0;
    char path[512];

    (void)m2sdr_get_fpga_dna(dev, &dna);
    if (m2sdr_cal_flash_load(dev, flash_offset, rec) == 0 && (rec->fpga_dna == dna || rec->fpga_dna == 0))
        return M2SDR_CAL_SOURCE_FLASH;
    if (m2sdr_cal_default_path(dna, path, sizeof(path)) == 0 &&
        m2sdr_cal_file_load(path, rec) == 0 && rec->fpga_dna == dna)
        return M2SDR_CAL_SOURCE_FILE;
    return M2SDR_CAL_SOURCE_NONE;
}
