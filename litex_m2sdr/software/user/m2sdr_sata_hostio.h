/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA host I/O engine.
 *
 * Streams sectors between the drive and host memory in staging-buffer-sized
 * chunks, decoupled from the concrete drive transport (PCIe DMA or Etherbone)
 * via a small drive-ops backend. Each transfer command (file copy, pattern
 * fill/check, export) is then just a direction plus a per-chunk callback.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_SATA_HOSTIO_H
#define M2SDR_SATA_HOSTIO_H

#include <stddef.h>
#include <stdint.h>

/* Drive backend: read/write `nsectors` 512-byte sectors at `sector`.
 * Returns 0 on success, non-zero on failure. */
struct sata_drive_ops {
    int (*read) (void *dev, uint64_t sector, uint32_t nsectors, uint8_t *buf, int timeout_ms);
    int (*write)(void *dev, uint64_t sector, uint32_t nsectors, const uint8_t *buf, int timeout_ms);
};

struct sata_host_io {
    void                        *dev;          /* Opaque handle passed to the ops.    */
    const struct sata_drive_ops *drive;        /* Drive transport backend.            */
    uint8_t                     *buf;          /* Staging buffer (max_sectors * 512). */
    uint32_t                     max_sectors;  /* Staging capacity, in sectors.       */
};

enum sata_host_io_dir {
    SATA_HOST_IO_READ,   /* Drive -> buffer, then the chunk callback consumes it. */
    SATA_HOST_IO_WRITE,  /* The chunk callback fills the buffer, then buffer -> drive. */
};

/* Per-chunk host-side hook. For WRITE it fills `buf` (the next `bytes` to send);
 * for READ it consumes `buf` (the `bytes` just read). `sector`/`nsectors` give
 * the current chunk's position. Returns 0 to continue, non-zero to abort. */
typedef int (*sata_host_io_chunk_fn)(uint8_t *buf, size_t bytes,
                                     uint64_t sector, uint32_t nsectors, void *ctx);

/* Allocate the staging buffer. Returns 0 on success, -1 on allocation failure. */
int  sata_host_io_init(struct sata_host_io *io, void *dev,
                       const struct sata_drive_ops *drive, uint32_t max_sectors);
void sata_host_io_cleanup(struct sata_host_io *io);

/* Stream `nsectors` starting at `sector` in chunks of <= max_sectors, invoking
 * `chunk` (may be NULL) on each. Returns 0 on success, non-zero on the first
 * drive error or callback abort. */
int  sata_host_io_run(struct sata_host_io *io, enum sata_host_io_dir dir,
                      uint64_t sector, uint32_t nsectors, int timeout_ms,
                      sata_host_io_chunk_fn chunk, void *ctx);

#endif /* M2SDR_SATA_HOSTIO_H */
