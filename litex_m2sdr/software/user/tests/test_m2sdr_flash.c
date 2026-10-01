/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for the SPI flash Etherbone path, run against an in-memory fake
 * of the gateware and the flash chip.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csr.h"
#include "etherbone.h"
#include "litepcie_helpers.h"
#include "m2sdr_flash.h"

#define FAKE_FLASH_SIZE (1 << 24) /* reach of a 3-byte address */
#define PAGE            256
#define SECTOR          FLASH_SECTOR_SIZE
#define MAX_REPLIES     256

/* Minimum frame with preamble and gap: 84 bytes. */
#define FRAME_NS_1G     672
#define FRAME_NS_2G5    268
#define MIN_GAP_NS      2700 /* behind every start */

/* One eb_*() call is one record; SPI transfers finish at once. */
struct eb_connection {
    /* Gateware */
    uint32_t eth_config;
    uint32_t cs_n;
    uint32_t mosi_hi, mosi_lo;
    uint64_t miso;
    uint32_t replies[MAX_REPLIES];
    unsigned head, tail;

    /* Flash */
    uint8_t *mem;
    uint8_t cmd;
    unsigned nbytes; /* since chip-select fell */
    uint32_t addr;
    uint8_t page[PAGE];
    int wel;
    unsigned busy;   /* status reads before WIP clears */

    /* Checks */
    uint32_t lo, hi; /* where erases and programs may land */
    unsigned stray, erases, programs, wrdi;
    unsigned writes, drop_every, drop_until;
    int start_pending;
    unsigned since_start, min_spacing, max_replies;
};

static uint32_t rng = 1;

static void fill_random(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        rng = rng * 1103515245u + 12345u;
        buf[i] = rng >> 16;
    }
}

/* Idle, chip-select high, flash contents kept. */
static void fake_reset(struct eb_connection *f)
{
    uint8_t *mem        = f->mem;
    uint32_t eth_config = f->eth_config;

    memset(f, 0, sizeof(*f));
    f->mem         = mem;
    f->eth_config  = eth_config;
    f->cs_n        = 1;
    f->min_spacing = ~0u;
}

static struct eb_connection *fake_new(uint32_t eth_speed)
{
    struct eb_connection *f = calloc(1, sizeof(*f));

    if (!f)
        return NULL;
    f->mem = malloc(FAKE_FLASH_SIZE);
    if (!f->mem) {
        free(f);
        return NULL;
    }
    memset(f->mem, 0xa5, FAKE_FLASH_SIZE);
    f->eth_config = eth_speed << CSR_CAPABILITY_ETH_CONFIG_SPEED_OFFSET;
    fake_reset(f);
    return f;
}

static void fake_free(struct eb_connection *f)
{
    if (f)
        free(f->mem);
    free(f);
}

/* Records after a start, up to the first one that needs it finished. */
static void fake_frame(struct eb_connection *f, int needs_idle, int start)
{
    if (f->start_pending) {
        f->since_start++;
        if (needs_idle) {
            if (f->since_start < f->min_spacing)
                f->min_spacing = f->since_start;
            f->start_pending = 0;
        }
    }
    if (start) {
        f->start_pending = 1;
        f->since_start   = 0;
    }
}

static void fake_land(struct eb_connection *f, uint32_t addr, uint32_t len)
{
    if (addr < f->lo || addr + len > f->hi)
        f->stray++;
}

static uint8_t fake_flash_byte(struct eb_connection *f, uint8_t in)
{
    unsigned n = f->nbytes++;

    if (n == 0) {
        f->cmd  = (f->busy && in != FLASH_RDSR) ? 0 : in; /* busy: RDSR only */
        f->addr = 0;
        memset(f->page, 0xff, sizeof(f->page));
        return 0xff;
    }
    switch (f->cmd) {
    case FLASH_RDSR: {
        uint8_t sr = (f->busy ? FLASH_WIP : 0) | (f->wel ? 0x02 : 0);

        if (f->busy)
            f->busy--;
        return sr;
    }
    case FLASH_READ:
    case FLASH_PP:
    case FLASH_SE:
        if (n <= 3) {
            f->addr = f->addr << 8 | in;
            return 0xff;
        }
        if (f->cmd == FLASH_READ)
            return f->mem[f->addr++ % FAKE_FLASH_SIZE];
        if (f->cmd == FLASH_PP)
            f->page[(f->addr + n - 4) % PAGE] = in;
        return 0xff;
    }
    return 0xff;
}

/* Whole commands run when chip-select rises. */
static void fake_deselect(struct eb_connection *f)
{
    unsigned n = f->nbytes;

    f->nbytes = 0;
    if (f->cmd == FLASH_WREN && n == 1) {
        f->wel = 1;
    } else if (f->cmd == FLASH_WRDI && n == 1) {
        f->wel = 0;
        f->wrdi++;
    } else if (f->cmd == FLASH_SE && n == 4 && f->wel) {
        uint32_t a = f->addr & ~(uint32_t)(SECTOR - 1);

        fake_land(f, a, SECTOR);
        memset(f->mem + a, 0xff, SECTOR);
        f->erases++;
        f->wel  = 0;
        f->busy = 1;
    } else if (f->cmd == FLASH_PP && n > 4 && f->wel) {
        uint32_t a = f->addr & ~(uint32_t)(PAGE - 1);

        fake_land(f, a, PAGE);
        for (unsigned i = 0; i < PAGE; i++)
            f->mem[a + i] &= f->page[i];
        f->programs++;
        f->wel  = 0;
        f->busy = 2;
    }
}

static void fake_transfer(struct eb_connection *f, unsigned bits)
{
    uint64_t mosi = (uint64_t)(f->mosi_hi & 0xff) << 32 | f->mosi_lo;

    f->miso = 0;
    for (unsigned i = 0; i < bits / 8; i++) {
        uint8_t in = mosi >> (32 - 8 * i);

        f->miso = f->miso << 8 | (f->cs_n ? 0xff : fake_flash_byte(f, in));
    }
}

static uint32_t fake_read(struct eb_connection *f, uint32_t addr)
{
    fake_frame(f, addr == CSR_FLASH_SPI_STATUS_ADDR || addr == CSR_FLASH_SPI_MISO_ADDR ||
                  addr == CSR_FLASH_SPI_MISO_ADDR + 4, 0);
    switch (addr) {
    case CSR_FLASH_SPI_STATUS_ADDR:
        return SPI_STATUS_DONE;
    case CSR_FLASH_SPI_MISO_ADDR + 0:
        return (uint32_t)(f->miso >> 32);
    case CSR_FLASH_SPI_MISO_ADDR + 4:
        return (uint32_t)f->miso;
    case CSR_CAPABILITY_ETH_CONFIG_ADDR:
        return f->eth_config;
    }
    return 0;
}

/* Etherbone */

void eb_write32(struct eb_connection *f, uint32_t val, uint32_t addr)
{
    int start = addr == CSR_FLASH_SPI_CONTROL_ADDR && (val & SPI_CTRL_START);

    fake_frame(f, start || addr == CSR_FLASH_CS_N_OUT_ADDR, start);
    f->writes++;
    if (f->drop_every && f->writes <= f->drop_until && f->writes % f->drop_every == 0)
        return; /* lost */

    switch (addr) {
    case CSR_FLASH_CS_N_OUT_ADDR:
        if (val && !f->cs_n)
            fake_deselect(f);
        else if (!val && f->cs_n)
            f->nbytes = 0;
        f->cs_n = !!val;
        break;
    case CSR_FLASH_SPI_MOSI_ADDR + 0:
        f->mosi_hi = val;
        break;
    case CSR_FLASH_SPI_MOSI_ADDR + 4:
        f->mosi_lo = val;
        break;
    case CSR_FLASH_SPI_CONTROL_ADDR:
        if (start)
            fake_transfer(f, (val / SPI_CTRL_LENGTH) & 0xff);
        break;
    }
}

int eb_send_read32(struct eb_connection *f, uint32_t addr)
{
    if (f->tail - f->head == MAX_REPLIES)
        return EB_ERR_IO;
    f->replies[f->tail++ % MAX_REPLIES] = fake_read(f, addr);
    if (f->tail - f->head > f->max_replies)
        f->max_replies = f->tail - f->head;
    return EB_ERR_OK;
}

int eb_recv_read32(struct eb_connection *f, uint32_t *val)
{
    if (f->head == f->tail)
        return EB_ERR_TIMEOUT;
    *val = f->replies[f->head++ % MAX_REPLIES];
    return EB_ERR_OK;
}

void eb_drain(struct eb_connection *f)
{
    f->head = f->tail;
}

int eb_is_direct(struct eb_connection *f)
{
    (void)f;
    return 1;
}

int eb_read32_checked(struct eb_connection *f, uint32_t addr, uint32_t *val)
{
    eb_drain(f);
    *val = fake_read(f, addr);
    return EB_ERR_OK;
}

uint32_t eb_read32(struct eb_connection *f, uint32_t addr)
{
    eb_drain(f);
    return fake_read(f, addr);
}

int64_t get_time_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The handle is never a PCIe file descriptor here. */
uint32_t litepcie_readl(int fd, uint32_t addr)
{
    (void)fd;
    (void)addr;
    abort();
}

void litepcie_writel(int fd, uint32_t addr, uint32_t val)
{
    (void)fd;
    (void)addr;
    (void)val;
    abort();
}

/* Helpers */

static int flash_write(struct eb_connection *f, uint32_t base, uint8_t *img, uint32_t size)
{
    f->lo = base;
    f->hi = base + (size + SECTOR - 1) / SECTOR * SECTOR;
    return m2sdr_flash_write(f, img, base, size, NULL, NULL);
}

/* img in the range, the rest of its sectors erased, nothing else touched. */
static int flash_holds(const struct eb_connection *f, uint32_t base, const uint8_t *img,
                       uint32_t size)
{
    uint32_t end = base + (size + SECTOR - 1) / SECTOR * SECTOR;

    if (memcmp(f->mem + base, img, size) != 0)
        return 0;
    for (uint32_t a = 0; a < FAKE_FLASH_SIZE; a++) {
        if (a >= base && a < base + size)
            continue;
        if (f->mem[a] != (a >= base && a < end ? 0xff : 0xa5))
            return 0;
    }
    return 1;
}

static unsigned pages_to_program(const uint8_t *img, uint32_t size)
{
    unsigned n = 0;

    for (uint32_t p = 0; p < size; p += PAGE) {
        for (uint32_t i = p; i < p + PAGE && i < size; i++) {
            if (img[i] != 0xff) {
                n++;
                break;
            }
        }
    }
    return n;
}

static int spacing_ok(const struct eb_connection *f, unsigned frame_ns)
{
    return f->min_spacing != ~0u && f->min_spacing * frame_ns >= MIN_GAP_NS;
}

/* Tests */

/* Mixed content at 1 Gb/s, chip-select asserted mid-command as after
 * configuration. */
static int test_write_1g(void)
{
    const uint32_t base = 2 * SECTOR, size = 2 * SECTOR + 4096 + 101;
    static const unsigned erased[] = {3, 4, 100, 256, 300, 520};
    struct eb_connection *f = fake_new(0);
    uint8_t *img  = malloc(size);
    uint8_t *back = malloc(size);
    int rc = -1;

    if (!f || !img || !back)
        goto out;
    fill_random(img, size);
    for (size_t i = 0; i < sizeof(erased) / sizeof(erased[0]); i++)
        memset(img + erased[i] * PAGE, 0xff, PAGE);
    memset(img + 11 * PAGE, 0x00, PAGE);
    memset(img + 101 * PAGE, 0xff, PAGE - 1);   /* last byte only */
    memset(img + 102 * PAGE + 1, 0xff, PAGE - 1); /* first byte only */
    f->cs_n   = 0;
    f->nbytes = 1;

    if (flash_write(f, base, img, size) != 0 || !flash_holds(f, base, img, size))
        goto out;
    if (f->stray || f->wrdi || f->erases != 3 || f->programs != pages_to_program(img, size))
        goto out;
    if (m2sdr_flash_read_range(f, base + 3, back, size - 7) != 0 ||
        memcmp(back, img + 3, size - 7) != 0)
        goto out;
    if (!spacing_ok(f, FRAME_NS_1G) || f->max_replies > 32)
        goto out;
    rc = 0;
out:
    free(back);
    free(img);
    fake_free(f);
    return rc;
}

/* Shorter frames at 2.5 Gb/s: the gap needs more of them. */
static int test_write_2g5(void)
{
    const uint32_t base = 5 * SECTOR, size = SECTOR + 3 * PAGE;
    struct eb_connection *f = fake_new(1);
    uint8_t *img = malloc(size);
    int rc = -1;

    if (!f || !img)
        goto out;
    fill_random(img, size);
    if (flash_write(f, base, img, size) != 0 || !flash_holds(f, base, img, size))
        goto out;
    if (f->stray || f->wrdi || !spacing_ok(f, FRAME_NS_2G5))
        goto out;
    rc = 0;
out:
    free(img);
    fake_free(f);
    return rc;
}

/* Lost writes corrupt a sector; verify catches it and the next pass rewrites
 * it. */
static int test_write_lost_packets(void)
{
    const uint32_t base = 8 * SECTOR, size = 2 * SECTOR;
    struct eb_connection *f = fake_new(0);
    uint8_t *img = malloc(size);
    int rc = -1;

    if (!f || !img)
        goto out;
    fill_random(img, size);
    f->drop_every = 997;
    f->drop_until = 60000;
    if (flash_write(f, base, img, size) != 0 || !flash_holds(f, base, img, size))
        goto out;
    if (f->stray || f->erases <= 2)
        goto out;
    rc = 0;
out:
    free(img);
    fake_free(f);
    return rc;
}

/* Each write of a page program lost in turn: nothing lands outside the range. */
static int test_write_each_write_lost(void)
{
    const uint32_t base = 13 * SECTOR, size = PAGE;
    struct eb_connection *f = fake_new(0);
    uint8_t img[PAGE];
    unsigned writes;
    int rc = -1;

    if (!f)
        goto out;
    fill_random(img, size);
    if (flash_write(f, base, img, size) != 0)
        goto out;
    writes = f->writes;
    for (unsigned k = 1; k <= writes; k++) {
        fake_reset(f);
        memset(f->mem + base, 0xa5, SECTOR);
        f->drop_every = k;
        f->drop_until = k;
        if (flash_write(f, base, img, size) != 0 || f->stray ||
            memcmp(f->mem + base, img, size) != 0) {
            fprintf(stderr, "write %u of %u lost\n", k, writes);
            goto out;
        }
    }
    rc = 0;
out:
    fake_free(f);
    return rc;
}

/* Unknown link speed and M2SDR_FLASH_LEGACY take the generic path, the only
 * one sending WRDI. */
static int test_generic_fallback(void)
{
    const uint32_t base = 11 * SECTOR, size = PAGE;
    struct eb_connection *f = fake_new(2);
    uint8_t *img = malloc(size);
    int rc = -1;
    int err;

    if (!f || !img)
        goto out;
    fill_random(img, size);
    if (flash_write(f, base, img, size) != 0 || !flash_holds(f, base, img, size) || !f->wrdi)
        goto out;

    fake_free(f);
    f = fake_new(0);
    if (!f)
        goto out;
    setenv("M2SDR_FLASH_LEGACY", "1", 1);
    err = flash_write(f, base, img, size);
    unsetenv("M2SDR_FLASH_LEGACY");
    if (err != 0 || !flash_holds(f, base, img, size) || !f->wrdi)
        goto out;
    rc = 0;
out:
    free(img);
    fake_free(f);
    return rc;
}

int main(void)
{
    if (test_write_1g() != 0) {
        fprintf(stderr, "test_write_1g failed\n");
        return 1;
    }
    if (test_write_2g5() != 0) {
        fprintf(stderr, "test_write_2g5 failed\n");
        return 1;
    }
    if (test_write_lost_packets() != 0) {
        fprintf(stderr, "test_write_lost_packets failed\n");
        return 1;
    }
    if (test_write_each_write_lost() != 0) {
        fprintf(stderr, "test_write_each_write_lost failed\n");
        return 1;
    }
    if (test_generic_fallback() != 0) {
        fprintf(stderr, "test_generic_fallback failed\n");
        return 1;
    }

    printf("test_m2sdr_flash: ok\n");
    return 0;
}
