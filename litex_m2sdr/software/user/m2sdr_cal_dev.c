/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR reference clock calibration: record in the SPI flash of a board.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <string.h>

#include "m2sdr.h"
#include "m2sdr_flash.h"
#include "m2sdr_cal_store.h"

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
