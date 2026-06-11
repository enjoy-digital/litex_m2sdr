/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA host I/O engine.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>

#include "m2sdr_sata_hostio.h"
#include "m2sdr_sata_lowlevel.h" /* SATA_SECTOR_BYTES */

int sata_host_io_init(struct sata_host_io *io, void *dev,
                      const struct sata_drive_ops *drive, uint32_t max_sectors)
{
    io->dev         = dev;
    io->drive       = drive;
    io->max_sectors = max_sectors;
    io->buf         = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    return io->buf ? 0 : -1;
}

void sata_host_io_cleanup(struct sata_host_io *io)
{
    free(io->buf);
    io->buf = NULL;
}

int sata_host_io_run(struct sata_host_io *io, enum sata_host_io_dir dir,
                     uint64_t sector, uint32_t nsectors, int timeout_ms,
                     sata_host_io_chunk_fn chunk, void *ctx)
{
    for (uint32_t done = 0; done < nsectors; ) {
        uint32_t n   = nsectors - done;
        uint64_t cur = sector + done;
        size_t   bytes;
        int      rc;

        if (n > io->max_sectors)
            n = io->max_sectors;
        bytes = (size_t)n * SATA_SECTOR_BYTES;

        if (dir == SATA_HOST_IO_WRITE) {
            if (chunk && (rc = chunk(io->buf, bytes, cur, n, ctx)) != 0)
                return rc;
            if (io->drive->write(io->dev, cur, n, io->buf, timeout_ms) != 0)
                return 1;
        } else {
            if (io->drive->read(io->dev, cur, n, io->buf, timeout_ms) != 0)
                return 1;
            if (chunk && (rc = chunk(io->buf, bytes, cur, n, ctx)) != 0)
                return rc;
        }
        done += n;
    }
    return 0;
}
