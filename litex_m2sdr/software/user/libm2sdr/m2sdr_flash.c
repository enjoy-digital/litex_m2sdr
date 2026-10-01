/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (C) 2024-2026 Enjoy-Digital
 *
 */

/* Includes */
/*----------*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "csr.h"
#include "soc.h"
#include "libm2sdr.h"
#include "m2sdr_flash.h"

#ifdef CSR_FLASH_BASE

/* Defines */
/*---------*/

#define FLASH_RETRIES            16
#define FLASH_PAGE_SIZE          256
#define FLASH_SECTOR_SIZE        (1 << 16)

#define SPI_TIMEOUT 100000 /* in us */
#define SPI_TRANSACTION_TIME_US  25
#define FLASH_ERASE_TIMEOUT_US   (10 * 1000 * 1000)
#define FLASH_PROGRAM_TIMEOUT_US (1 * 1000 * 1000)
#define FLASH_PROGRESS_STEP_US   (250 * 1000)

/* The flash bridge exposes a small shift-register style SPI engine. These
 * helpers wrap it so the higher-level write path can stay transport-agnostic. */

/* flash_spi_cs */
/*--------------*/

static void flash_spi_cs(void *conn, uint8_t cs_n)
{
    m2sdr_writel(conn, CSR_FLASH_CS_N_OUT_ADDR, cs_n);
}

static void flash_wait_done(void *conn, const char *op, uint8_t cmd, int tx_len)
{
    uint32_t status = 0;

    for (int i = 0; i < SPI_TIMEOUT; i++) {
        status = m2sdr_readl(conn, CSR_FLASH_SPI_STATUS_ADDR);
        if (status & SPI_STATUS_DONE)
            return;
        usleep(1);
    }

    fprintf(stderr,
        "\nTimeout waiting for SPI done during %s (cmd=0x%02x, len=%d, status=0x%08x)\n",
        op, cmd, tx_len, status);
    abort();
}

/* flash_spi */
/*-----------*/

static uint64_t flash_spi(void *conn, int tx_len, uint8_t cmd, uint32_t tx_data)
{
    uint64_t tx = ((uint64_t)cmd << 32) | tx_data;
    uint64_t rx = 0;

    if (tx_len < 8 || tx_len > 40) {
        fprintf(stderr, "Invalid SPI transaction length: %d\n", tx_len);
        return 0;
    }

    /* The bridge expects chip-select to be driven explicitly around each
     * logical SPI command. */
    flash_spi_cs(conn, 0);

    m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 0, (tx >> 32) & 0xffffffff);
    m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, (tx >>  0) & 0xffffffff);
    m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR,
                 SPI_CTRL_START | (tx_len * SPI_CTRL_LENGTH));

    if (m2sdr_legacy_handle_is_fd(conn)) {
        /* Poll SPI_STATUS_DONE for PCIe. */
        flash_wait_done(conn, "flash_spi", cmd, tx_len);
        rx = ((uint64_t)m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR) << 32) |
              m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR + 4);
    } else {
        /* Etherbone already pays a network latency cost, so a short fixed
         * delay is sufficient here instead of polling a local completion bit. */
        usleep(SPI_TRANSACTION_TIME_US);
        if (tx_len > 8)
            rx = m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR + 4);
    }

    flash_spi_cs(conn, 1);

    return rx;
}

/* flash_read_id */
/*---------------*/

uint32_t flash_read_id(void *conn, int reg)
{
    return flash_spi(conn, 32, reg, 0) & 0xffffff;
}

/* flash_write_enable */
/*--------------------*/

static void flash_write_enable(void *conn)
{
    /* Flash program/erase operations require an explicit write-enable latch. */
    flash_spi(conn, 8, FLASH_WREN, 0);
}

/* flash_write_disable */
/*---------------------*/

static void flash_write_disable(void *conn)
{
    flash_spi(conn, 8, FLASH_WRDI, 0);
}

/* flash_read_status */
/*-------------------*/

static uint8_t flash_read_status(void *conn)
{
    /* The status register drives the WIP polling loops during erase/program. */
    return flash_spi(conn, 16, FLASH_RDSR, 0) & 0xff;
}

/* flash_erase_sector */
/*--------------------*/

static void flash_erase_sector(void *conn, uint32_t addr)
{
    flash_spi(conn, 32, FLASH_SE, addr << 8);
}

static int flash_wait_while_busy(void *conn, uint32_t addr, const char *op,
                                 unsigned timeout_us, unsigned poll_us,
                                 void (*progress_cb)(void *opaque, const char *fmt, ...),
                                 void *opaque)
{
    int64_t start_ms = get_time_ms();
    unsigned next_report_us = FLASH_PROGRESS_STEP_US;
    uint8_t status;

    while ((status = flash_read_status(conn)) & FLASH_WIP) {
        int64_t elapsed_ms = get_time_ms() - start_ms;
        unsigned elapsed_us = (unsigned)(elapsed_ms * 1000);

        if (progress_cb && elapsed_us >= next_report_us) {
            progress_cb(opaque, "%s @%08x... status=0x%02x elapsed=%lldms\r",
                        op, addr, status, (long long)elapsed_ms);
            next_report_us += FLASH_PROGRESS_STEP_US;
        }
        if (elapsed_us >= timeout_us) {
            fprintf(stderr,
                "\nTimeout waiting for flash %s @0x%08x, status=0x%02x after %lldms\n",
                op, addr, status, (long long)elapsed_ms);
            return 1;
        }
        if (poll_us)
            usleep(poll_us);
    }

    return 0;
}

/* flash_write_buffer */
/*--------------------*/

static void flash_write_buffer(void *conn, uint32_t addr, uint8_t *buf, uint16_t size)
{
    /* Program in page-sized chunks using the bridge's fixed-width SPI words. */
    if (size == 1) {
        flash_spi(conn, 40, FLASH_PP, (addr << 8) | buf[0]);
    } else {
        int i;
        uint64_t tx;

        flash_spi_cs(conn, 0);

        /* send command+addr */
        tx = ((uint64_t)FLASH_PP << 32) | ((uint64_t)addr << 8);
        m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 0, (tx >> 32) & 0xffffffff);
        m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, (tx >>  0) & 0xffffffff);
        m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR,
                     SPI_CTRL_START | (32 * SPI_CTRL_LENGTH));

        if (m2sdr_legacy_handle_is_fd(conn))
            flash_wait_done(conn, "flash_write_buffer_cmd", FLASH_PP, 32);
        else
            usleep(SPI_TRANSACTION_TIME_US);

        /* send data words */
        for (i = 0; i < size; i += 4) {
            tx = ((uint64_t)buf[i + 0] << 32) |
                 ((uint64_t)buf[i + 1] << 24) |
                 ((uint64_t)buf[i + 2] << 16) |
                 ((uint64_t)buf[i + 3] <<  8);
            m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 0, (tx >> 32) & 0xffffffff);
            m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, (tx >>  0) & 0xffffffff);
            m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR,
                         SPI_CTRL_START | (32 * SPI_CTRL_LENGTH));

            if (m2sdr_legacy_handle_is_fd(conn))
                flash_wait_done(conn, "flash_write_buffer_data", FLASH_PP, 32);
            else
                usleep(SPI_TRANSACTION_TIME_US);
        }

        flash_spi_cs(conn, 1);
    }
}

/* m2sdr_flash_read */
/*------------------*/

uint8_t m2sdr_flash_read(void *conn, uint32_t addr)
{
    return flash_spi(conn, 40, FLASH_READ, addr << 8) & 0xff;
}

/* m2sdr_flash_read_buffer */
/*-------------------------*/

static void m2sdr_flash_read_buffer(void *conn, uint32_t addr, uint8_t *buf, uint16_t size)
{
    int i;
    uint64_t tx, rx;

    if (size == 1) {
        buf[0] = m2sdr_flash_read(conn, addr);
        return;
    }

    /* Keep chip-select asserted across the command and dummy reads so the
     * flash remains in continuous-read mode. */
    flash_spi_cs(conn, 0);

    tx = ((uint64_t)FLASH_READ << 32) | ((uint64_t)addr << 8);
    m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 0, (tx >> 32) & 0xffffffff);
    m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, (tx >>  0) & 0xffffffff);
    m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR,
                 SPI_CTRL_START | (32 * SPI_CTRL_LENGTH));

    if (m2sdr_legacy_handle_is_fd(conn))
        flash_wait_done(conn, "flash_read_buffer_cmd", FLASH_READ, 32);
    else
        usleep(SPI_TRANSACTION_TIME_US);

    for (i = 0; i < size; i += 4) {
        m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 0, 0);
        m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, 0);
        m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR,
                     SPI_CTRL_START | (32 * SPI_CTRL_LENGTH));

        if (m2sdr_legacy_handle_is_fd(conn))
            flash_wait_done(conn, "flash_read_buffer_data", FLASH_READ, 32);
        else
            usleep(SPI_TRANSACTION_TIME_US);

        rx = (uint64_t)m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR + 4);
        buf[i + 0] = (rx >> 24) & 0xff;
        buf[i + 1] = (rx >> 16) & 0xff;
        buf[i + 2] = (rx >>  8) & 0xff;
        buf[i + 3] = (rx >>  0) & 0xff;
    }

    flash_spi_cs(conn, 1);
}

/* m2sdr_flash_get_erase_block_size */
/*----------------------------------*/

int m2sdr_flash_get_erase_block_size(void *conn)
{
    (void)conn;
    return FLASH_SECTOR_SIZE;
}

/* m2sdr_flash_get_flash_program_size */
/*-----------------------------------*/

static int m2sdr_flash_get_flash_program_size(void *conn)
{
    (void)conn;
    return FLASH_PAGE_SIZE;
}

#ifdef USE_LITEETH

/* Etherbone fast path */
/*---------------------*/

/* The generic path waits on every packet, about 7 KB/s. Here packets go out
 * without waiting and the range is verified at the end. Records stay single
 * reads or writes: older gateware hangs on mixed ones.
 *
 * The bridge ignores a start while a transfer runs (1.6 us for 40 bits at
 * 25 MHz). Host timing does not survive a USB NIC, but frames on the link are
 * at least 0.67 us apart at 1 Gb/s, 0.27 us at 2.5 Gb/s, so each start is
 * followed by a gap of MOSI-only writes. */

#define FLASH_EB_GAP_1G        4   /* frames, 2.7 us */
#define FLASH_EB_GAP_2G5       10  /* frames, 2.7 us */
#define FLASH_EB_BURST_WORDS   16  /* 32 queued replies fit a default socket buffer */
#define FLASH_EB_READ_ATTEMPTS 4
#define FLASH_EB_WRITE_PASSES  4

struct flash_eb {
    struct eb_connection *eb;
    unsigned gap; /* MOSI-only frames after each start */
};

/* Unknown link speeds and M2SDR_FLASH_LEGACY take the generic path.
 * M2SDR_FLASH_GAP_FRAMES overrides the gap. */
static int flash_eb_open(void *conn, struct flash_eb *fe)
{
    const char *gap = getenv("M2SDR_FLASH_GAP_FRAMES");
    uint32_t eth_config;

    if (m2sdr_legacy_handle_is_fd(conn) || !eb_is_direct(conn) || getenv("M2SDR_FLASH_LEGACY"))
        return 0;
    if (eb_read32_checked(conn, CSR_CAPABILITY_ETH_CONFIG_ADDR, &eth_config) != EB_ERR_OK)
        return 0;
    switch ((eth_config >> CSR_CAPABILITY_ETH_CONFIG_SPEED_OFFSET) &
            ((1 << CSR_CAPABILITY_ETH_CONFIG_SPEED_SIZE) - 1)) {
    case 0:
        fe->gap = FLASH_EB_GAP_1G;
        break;
    case 1:
        fe->gap = FLASH_EB_GAP_2G5;
        break;
    default:
        return 0;
    }
    if (gap && atoi(gap) > 0)
        fe->gap = (unsigned)atoi(gap);
    fe->eb = conn;
    return 1;
}

static void flash_eb_cs(const struct flash_eb *fe, uint32_t cs_n)
{
    eb_write32(fe->eb, cs_n, CSR_FLASH_CS_N_OUT_ADDR);
}

static void flash_eb_load(const struct flash_eb *fe, uint32_t hi, uint32_t lo)
{
    eb_write32(fe->eb, hi, CSR_FLASH_SPI_MOSI_ADDR + 0);
    eb_write32(fe->eb, lo, CSR_FLASH_SPI_MOSI_ADDR + 4);
}

/* Start a transfer, then load the next one until the gap is filled. */
static void flash_eb_start(const struct flash_eb *fe, int bits, uint32_t next_hi,
                           uint32_t next_lo)
{
    unsigned gap = fe->gap < 2 ? 2 : fe->gap; /* both halves get loaded */

    eb_write32(fe->eb, SPI_CTRL_START | (bits * SPI_CTRL_LENGTH), CSR_FLASH_SPI_CONTROL_ADDR);
    eb_write32(fe->eb, next_hi, CSR_FLASH_SPI_MOSI_ADDR + 0);
    for (unsigned i = 1; i < gap; i++)
        eb_write32(fe->eb, next_lo, CSR_FLASH_SPI_MOSI_ADDR + 4);
}

/* The address stays in MOSI from the write-enable on, so a lost load cannot
 * leave zero there and aim an erase at the fallback image. */

static void flash_eb_write_enable(const struct flash_eb *fe, uint8_t next_cmd, uint32_t addr)
{
    flash_eb_cs(fe, 0);
    flash_eb_load(fe, FLASH_WREN, addr << 8);
    flash_eb_start(fe, 8, next_cmd, addr << 8);
    flash_eb_cs(fe, 1);
}

static void flash_eb_erase_sector(const struct flash_eb *fe, uint32_t addr)
{
    flash_eb_write_enable(fe, FLASH_SE, addr);
    flash_eb_cs(fe, 0);
    flash_eb_start(fe, 32, FLASH_SE, addr << 8);
    flash_eb_cs(fe, 1);
}

static void flash_eb_program_page(const struct flash_eb *fe, uint32_t addr, const uint8_t *page)
{
    flash_eb_write_enable(fe, FLASH_PP, addr);
    flash_eb_cs(fe, 0);
    for (int i = 0; i < FLASH_PAGE_SIZE; i += 4) /* the command, then each previous word */
        flash_eb_start(fe, 32, page[i],
                       (uint32_t)page[i + 1] << 24 | (uint32_t)page[i + 2] << 16 |
                       (uint32_t)page[i + 3] << 8);
    flash_eb_start(fe, 32, FLASH_PP, addr << 8);
    flash_eb_cs(fe, 1);
}

static int flash_eb_read_status(const struct flash_eb *fe, uint32_t addr, uint8_t *sr)
{
    uint32_t status, data;

    eb_drain(fe->eb);
    flash_eb_cs(fe, 0);
    flash_eb_load(fe, FLASH_RDSR, addr << 8);
    flash_eb_start(fe, 16, FLASH_RDSR, addr << 8);
    eb_send_read32(fe->eb, CSR_FLASH_SPI_STATUS_ADDR);
    eb_send_read32(fe->eb, CSR_FLASH_SPI_MISO_ADDR + 4);
    flash_eb_cs(fe, 1);
    if (eb_recv_read32(fe->eb, &status) != EB_ERR_OK ||
        eb_recv_read32(fe->eb, &data) != EB_ERR_OK || status != SPI_STATUS_DONE)
        return -1;
    *sr = data & 0xff;
    return 0;
}

static int flash_eb_wait_ready(const struct flash_eb *fe, uint32_t addr, const char *op,
                               unsigned timeout_us, unsigned poll_us)
{
    int64_t start_ms = get_time_ms();
    uint8_t sr = 0;

    for (;;) {
        if (flash_eb_read_status(fe, addr, &sr) == 0 && !(sr & FLASH_WIP))
            return 0;
        if ((get_time_ms() - start_ms) * 1000 > timeout_us) {
            fprintf(stderr, "\nTimeout waiting for flash %s @0x%08x, status=0x%02x\n",
                    op, addr, sr);
            return 1;
        }
        if (poll_us)
            usleep(poll_us);
    }
}

/* Replies are collected after the last request. A data word only counts
 * behind a done status. The flash ignores MOSI while it streams out. */
static int flash_eb_read_burst(const struct flash_eb *fe, uint32_t addr, uint8_t *out,
                               size_t words)
{
    if (words == 0 || words > FLASH_EB_BURST_WORDS)
        return -1;

    eb_drain(fe->eb);
    flash_eb_cs(fe, 0);
    flash_eb_load(fe, FLASH_READ, addr << 8);
    flash_eb_start(fe, 32, FLASH_READ, addr << 8); /* the command */
    for (size_t k = 0; k < words; k++) {
        flash_eb_start(fe, 32, FLASH_READ, addr << 8);
        eb_send_read32(fe->eb, CSR_FLASH_SPI_STATUS_ADDR);
        eb_send_read32(fe->eb, CSR_FLASH_SPI_MISO_ADDR + 4);
    }
    flash_eb_cs(fe, 1);

    for (size_t k = 0; k < words; k++) {
        uint32_t status, data;

        if (eb_recv_read32(fe->eb, &status) != EB_ERR_OK ||
            eb_recv_read32(fe->eb, &data) != EB_ERR_OK || status != SPI_STATUS_DONE)
            return -1;
        out[4 * k + 0] = data >> 24;
        out[4 * k + 1] = data >> 16;
        out[4 * k + 2] = data >> 8;
        out[4 * k + 3] = data;
    }
    return 0;
}

/* size must be a multiple of 4. */
static int flash_eb_read(const struct flash_eb *fe, uint32_t addr, uint8_t *out, uint32_t size)
{
    for (uint32_t off = 0; off < size; off += 4 * FLASH_EB_BURST_WORDS) {
        size_t words = (size - off) / 4;
        int attempt;

        if (words > FLASH_EB_BURST_WORDS)
            words = FLASH_EB_BURST_WORDS;
        for (attempt = 0; attempt < FLASH_EB_READ_ATTEMPTS; attempt++) {
            if (flash_eb_read_burst(fe, addr + off, out + off, words) == 0)
                break;
            usleep(10000); /* let stragglers land before the drain */
        }
        if (attempt == FLASH_EB_READ_ATTEMPTS)
            return -1;
    }
    return 0;
}

/* Chip-select comes out of configuration asserted and the first command is
 * lost; a transfer with the flash deselected gives the next a clean edge. */
static void flash_eb_begin(const struct flash_eb *fe, uint32_t addr)
{
    flash_eb_cs(fe, 1);
    flash_eb_start(fe, 8, 0, addr << 8);
}

static int flash_eb_page_erased(const uint8_t *page)
{
    for (int i = 0; i < FLASH_PAGE_SIZE; i++)
        if (page[i] != 0xff)
            return 0;
    return 1;
}

/* A sector that reads back wrong twice is rewritten on the next pass. */
static int flash_eb_write(const struct flash_eb *fe, const uint8_t *buf, uint32_t base,
                          uint32_t size,
                          void (*progress_cb)(void *opaque, const char *fmt, ...),
                          void *opaque)
{
    uint32_t sectors = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    uint8_t *pending = malloc(sectors);
    uint8_t *check   = malloc(FLASH_SECTOR_SIZE);
    uint32_t left    = sectors;

    if (!pending || !check) {
        free(pending);
        free(check);
        return 1;
    }
    memset(pending, 1, sectors);
    flash_eb_begin(fe, base);

    for (int pass = 0; pass < FLASH_EB_WRITE_PASSES && left; pass++) {
        for (uint32_t s = 0; s < sectors; s++) {
            uint32_t addr = base + s * FLASH_SECTOR_SIZE;

            if (!pending[s])
                continue;
            if (progress_cb)
                progress_cb(opaque, "Erasing @%08x\r", addr);
            flash_eb_erase_sector(fe, addr);
            if (flash_eb_wait_ready(fe, addr, "Erasing", FLASH_ERASE_TIMEOUT_US, 1000) != 0)
                goto out;
        }
        if (progress_cb)
            progress_cb(opaque, "\n");

        for (uint32_t s = 0; s < sectors; s++) {
            uint32_t off = s * FLASH_SECTOR_SIZE;

            if (!pending[s])
                continue;
            if (progress_cb)
                progress_cb(opaque, "Writing @%08x\r", base + off);
            for (uint32_t p = off; p < off + FLASH_SECTOR_SIZE && p < size; p += FLASH_PAGE_SIZE) {
                const uint8_t *page = buf + p;
                uint8_t tail[FLASH_PAGE_SIZE];

                if (size - p < FLASH_PAGE_SIZE) { /* 0xFF programs nothing */
                    memset(tail, 0xff, sizeof(tail));
                    memcpy(tail, buf + p, size - p);
                    page = tail;
                }
                if (flash_eb_page_erased(page))
                    continue;
                flash_eb_program_page(fe, base + p, page);
                if (flash_eb_wait_ready(fe, base + p, "Writing", FLASH_PROGRAM_TIMEOUT_US, 0) != 0)
                    goto out;
            }
        }
        if (progress_cb)
            progress_cb(opaque, "\n");

        left = 0;
        for (uint32_t s = 0; s < sectors; s++) {
            uint32_t off = s * FLASH_SECTOR_SIZE;
            uint32_t len = size - off < FLASH_SECTOR_SIZE ? size - off : FLASH_SECTOR_SIZE;
            int ok = 0;

            if (!pending[s])
                continue;
            if (progress_cb)
                progress_cb(opaque, "Verifying @%08x\r", base + off);
            for (int tries = 0; tries < 2 && !ok; tries++)
                ok = flash_eb_read(fe, base + off, check, (len + 3) & ~3u) == 0 &&
                     memcmp(check, buf + off, len) == 0;
            pending[s] = !ok;
            left += !ok;
        }
        if (progress_cb && left)
            progress_cb(opaque, "\n%u sector(s) did not verify%s\n", left,
                        pass + 1 < FLASH_EB_WRITE_PASSES ? ", rewriting" : "");
        else if (progress_cb)
            progress_cb(opaque, "\n");
    }

out:
    free(pending);
    free(check);
    return (int)left;
}

#endif /* USE_LITEETH */

/* m2sdr_flash_write */
/*-------------------*/

int m2sdr_flash_write(void *conn,
                      uint8_t *buf, uint32_t base, uint32_t size,
                      void (*progress_cb)(void *opaque, const char *fmt, ...),
                      void *opaque)
{
    uint16_t prog_size = m2sdr_flash_get_flash_program_size(conn);
    uint8_t cmp_buf[FLASH_PAGE_SIZE];
    int i = 0;
    int retries = 0;

#ifdef USE_LITEETH
    struct flash_eb fe;

    if (flash_eb_open(conn, &fe))
        return flash_eb_write(&fe, buf, base, size, progress_cb, opaque);
#endif

    flash_read_id(conn, 0);
    flash_write_enable(conn);

    /* Erase all sectors that overlap the requested range first, then verify
     * each programmed page by reading it back. */
    /* Erase */
    for (i = 0; i < size; i += FLASH_SECTOR_SIZE) {
        if (progress_cb) {
            progress_cb(opaque, "Erasing @%08x\r", base + i);
        }
        flash_write_enable(conn);
        flash_erase_sector(conn, base + i);
        if (flash_wait_while_busy(conn, base + i, "Erasing", FLASH_ERASE_TIMEOUT_US, 1000,
                                  progress_cb, opaque) != 0)
            return 1;
    }
    if (progress_cb) {
        progress_cb(opaque, "\n");
    }

    flash_write_disable(conn);

    /* Program */
    i = 0;
    retries = 0;
    while (i < size) {
        if (progress_cb && (i % FLASH_SECTOR_SIZE) == 0) {
            progress_cb(opaque, "Writing @%08x\r", base + i);
        }

        if (flash_wait_while_busy(conn, base + i, "Waiting", FLASH_PROGRAM_TIMEOUT_US, 1000,
                                  progress_cb, opaque) != 0)
            return 1;

        flash_write_enable(conn);
        flash_write_buffer(conn, base + i, buf + i, prog_size);
        flash_write_disable(conn);

        if (flash_wait_while_busy(conn, base + i, "Writing", FLASH_PROGRAM_TIMEOUT_US, 1000,
                                  progress_cb, opaque) != 0)
            return 1;

        /* Read back each page immediately so user-space callers get a simple,
         * conservative "program and verify" behavior. */
        m2sdr_flash_read_buffer(conn, base + i, cmp_buf, prog_size);
        if (memcmp(buf + i, cmp_buf, prog_size) != 0) {
            retries++;
            if (retries > FLASH_RETRIES) {
                printf("Not able to write page\n");
                return 1;
            }
        } else {
            i += prog_size;
            retries = 0;
        }
    }

    if (progress_cb) {
        progress_cb(opaque, "\n");
    }

    return 0;
}

/* m2sdr_flash_read_range */
/*------------------------*/

int m2sdr_flash_read_range(void *conn, uint32_t addr, uint8_t *buf, uint32_t size)
{
    uint8_t page[FLASH_PAGE_SIZE];

#ifdef USE_LITEETH
    struct flash_eb fe;

    if (flash_eb_open(conn, &fe)) {
        /* A dump counts only once a second read agrees. */
        uint32_t padded = (size + 3) & ~3u;
        uint8_t *a = malloc(padded);
        uint8_t *b = malloc(padded);
        int ok = 0;

        flash_eb_begin(&fe, addr);
        for (int attempt = 0; a && b && attempt < FLASH_EB_READ_ATTEMPTS && !ok; attempt++)
            ok = flash_eb_read(&fe, addr, a, padded) == 0 &&
                 flash_eb_read(&fe, addr, b, padded) == 0 && memcmp(a, b, padded) == 0;
        if (ok)
            memcpy(buf, a, size);
        free(a);
        free(b);
        return ok ? 0 : -1;
    }
#endif

    for (uint32_t off = 0; off < size; off += FLASH_PAGE_SIZE) {
        uint32_t n = size - off < FLASH_PAGE_SIZE ? size - off : FLASH_PAGE_SIZE;

        m2sdr_flash_read_buffer(conn, addr + off, page, FLASH_PAGE_SIZE);
        memcpy(buf + off, page, n);
    }
    return 0;
}

#endif /* CSR_FLASH_BASE */
