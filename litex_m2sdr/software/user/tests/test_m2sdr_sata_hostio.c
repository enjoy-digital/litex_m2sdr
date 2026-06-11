/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for the SATA host I/O chunk-loop engine.
 *
 * The engine is exercised against an in-memory fake drive (no hardware), so
 * these tests cover the chunking arithmetic, partial last chunk, direction
 * ordering, and error/abort propagation that the real transfer commands rely on.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../m2sdr_sata_hostio.h"

#define SECTOR_BYTES 512u

/* In-memory fake drive ----------------------------------------------------- */

struct fake_drive {
    uint8_t *disk;
    uint32_t disk_sectors;
    int      reads;
    int      writes;
    int      fail_at_sector; /* return error when a chunk starts here (-1: never) */
};

static uint8_t expected_byte(uint64_t sector, size_t i)
{
    /* Deterministic function of the absolute byte offset. */
    return (uint8_t)((sector * SECTOR_BYTES + i) * 7u + 3u);
}

static int fake_read(void *dev, uint64_t sector, uint32_t nsectors, uint8_t *buf, int timeout_ms)
{
    struct fake_drive *d = dev;
    (void)timeout_ms;
    if (d->fail_at_sector >= 0 && sector == (uint64_t)d->fail_at_sector)
        return 1;
    assert(sector + nsectors <= d->disk_sectors);
    memcpy(buf, d->disk + sector * SECTOR_BYTES, (size_t)nsectors * SECTOR_BYTES);
    d->reads++;
    return 0;
}

static int fake_write(void *dev, uint64_t sector, uint32_t nsectors, const uint8_t *buf, int timeout_ms)
{
    struct fake_drive *d = dev;
    (void)timeout_ms;
    if (d->fail_at_sector >= 0 && sector == (uint64_t)d->fail_at_sector)
        return 1;
    assert(sector + nsectors <= d->disk_sectors);
    memcpy(d->disk + sector * SECTOR_BYTES, buf, (size_t)nsectors * SECTOR_BYTES);
    d->writes++;
    return 0;
}

static const struct sata_drive_ops fake_ops = { fake_read, fake_write };

static struct fake_drive g_drive;

static void drive_reset(uint32_t sectors)
{
    free(g_drive.disk);
    g_drive.disk           = calloc(sectors, SECTOR_BYTES);
    g_drive.disk_sectors   = sectors;
    g_drive.reads          = 0;
    g_drive.writes         = 0;
    g_drive.fail_at_sector = -1;
    assert(g_drive.disk);
}

/* Host-side chunk callbacks ------------------------------------------------ */

static int fill_cb(uint8_t *buf, size_t bytes, uint64_t sector, uint32_t nsectors, void *ctx)
{
    (void)nsectors; (void)ctx;
    for (size_t i = 0; i < bytes; i++)
        buf[i] = expected_byte(sector, i);
    return 0;
}

struct check_ctx { int ok; };

static int check_cb(uint8_t *buf, size_t bytes, uint64_t sector, uint32_t nsectors, void *ctx)
{
    struct check_ctx *c = ctx;
    (void)nsectors;
    for (size_t i = 0; i < bytes; i++) {
        if (buf[i] != expected_byte(sector, i)) {
            c->ok = 0;
            return 1;
        }
    }
    return 0;
}

struct rec { uint64_t sector; uint32_t nsectors; };
struct rec_ctx { struct rec recs[64]; int count; int abort_at; int abort_rc; };

static int rec_cb(uint8_t *buf, size_t bytes, uint64_t sector, uint32_t nsectors, void *ctx)
{
    struct rec_ctx *r = ctx;
    (void)buf; (void)bytes;
    r->recs[r->count].sector   = sector;
    r->recs[r->count].nsectors = nsectors;
    r->count++;
    if (r->abort_at >= 0 && r->count == r->abort_at)
        return r->abort_rc;
    return 0;
}

/* Tests -------------------------------------------------------------------- */

/* Write then read back, asserting byte-exact round-trip. */
static void test_roundtrip(uint64_t start, uint32_t nsectors, uint32_t max_sectors)
{
    struct sata_host_io io;
    struct check_ctx c = { 1 };

    drive_reset((uint32_t)start + nsectors);
    assert(sata_host_io_init(&io, &g_drive, &fake_ops, max_sectors) == 0);

    assert(sata_host_io_run(&io, SATA_HOST_IO_WRITE, start, nsectors, 1000, fill_cb, NULL) == 0);
    assert(sata_host_io_run(&io, SATA_HOST_IO_READ, start, nsectors, 1000, check_cb, &c) == 0);
    assert(c.ok);

    sata_host_io_cleanup(&io);
    assert(io.buf == NULL);
}

/* The chunk loop must split nsectors into <= max_sectors pieces at the right
 * sectors, with a partial final chunk. */
static void test_chunk_boundaries(void)
{
    struct sata_host_io io;
    struct rec_ctx r = { .count = 0, .abort_at = -1 };

    drive_reset(10);
    assert(sata_host_io_init(&io, &g_drive, &fake_ops, 4) == 0);

    assert(sata_host_io_run(&io, SATA_HOST_IO_READ, 0, 10, 1000, rec_cb, &r) == 0);
    assert(r.count == 3);
    assert(r.recs[0].sector == 0 && r.recs[0].nsectors == 4);
    assert(r.recs[1].sector == 4 && r.recs[1].nsectors == 4);
    assert(r.recs[2].sector == 8 && r.recs[2].nsectors == 2);
    assert(g_drive.reads == 3);

    sata_host_io_cleanup(&io);
}

/* A callback abort must stop the loop and propagate its return code; for WRITE
 * the drive must not be touched for the aborted chunk (callback runs first). */
static void test_callback_abort(void)
{
    struct sata_host_io io;
    struct rec_ctx r = { .count = 0, .abort_at = 2, .abort_rc = 42 };

    drive_reset(10);
    assert(sata_host_io_init(&io, &g_drive, &fake_ops, 4) == 0);

    assert(sata_host_io_run(&io, SATA_HOST_IO_WRITE, 0, 10, 1000, rec_cb, &r) == 42);
    assert(r.count == 2);        /* aborted on the second chunk's callback */
    assert(g_drive.writes == 1); /* only the first chunk was written       */

    sata_host_io_cleanup(&io);
}

/* A drive error must abort the run with a non-zero status. */
static void test_drive_error(void)
{
    struct sata_host_io io;

    drive_reset(10);
    assert(sata_host_io_init(&io, &g_drive, &fake_ops, 4) == 0);
    g_drive.fail_at_sector = 4; /* second chunk fails */

    assert(sata_host_io_run(&io, SATA_HOST_IO_READ, 0, 10, 1000, NULL, NULL) != 0);
    assert(g_drive.reads == 1); /* first chunk succeeded, second failed */

    sata_host_io_cleanup(&io);
}

/* A NULL callback streams straight to/from the drive. */
static void test_null_callback(void)
{
    struct sata_host_io io;

    drive_reset(6);
    assert(sata_host_io_init(&io, &g_drive, &fake_ops, 4) == 0);
    assert(sata_host_io_run(&io, SATA_HOST_IO_WRITE, 0, 6, 1000, NULL, NULL) == 0);
    assert(g_drive.writes == 2); /* 4 + 2 */
    sata_host_io_cleanup(&io);
}

int main(void)
{
    test_roundtrip(0,   10, 4);  /* partial last chunk           */
    test_roundtrip(0,    8, 8);  /* single full chunk            */
    test_roundtrip(0,    1, 4);  /* fewer sectors than buffer    */
    test_roundtrip(0,    7, 1);  /* one sector per chunk         */
    test_roundtrip(3,   13, 5);  /* non-zero start sector        */
    test_roundtrip(0,  100, 16); /* many chunks                  */

    test_chunk_boundaries();
    test_callback_abort();
    test_drive_error();
    test_null_callback();

    free(g_drive.disk);
    printf("test_m2sdr_sata_hostio: ok\n");
    return 0;
}
