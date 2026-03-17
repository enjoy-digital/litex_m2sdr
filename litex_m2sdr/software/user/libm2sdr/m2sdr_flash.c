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

#ifdef USE_LITEPCIE
    /* Poll SPI_STATUS_DONE for PCIe */
    flash_wait_done(conn, "flash_spi", cmd, tx_len);
    rx = ((uint64_t)m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR) << 32) |
          m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR + 4);
#endif

#ifdef USE_LITEETH
    /* Etherbone already pays a network latency cost, so a short fixed delay is
     * sufficient here instead of polling a local completion bit. */
    usleep(SPI_TRANSACTION_TIME_US);
    if (tx_len > 8) {
        rx = m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR + 4);
    }
#endif

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
                                 unsigned timeout_us,
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
        usleep(1000);
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

#ifdef USE_LITEPCIE
        flash_wait_done(conn, "flash_write_buffer_cmd", FLASH_PP, 32);
#endif
#ifdef USE_LITEETH
        usleep(SPI_TRANSACTION_TIME_US);
#endif

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

#ifdef USE_LITEPCIE
            flash_wait_done(conn, "flash_write_buffer_data", FLASH_PP, 32);
#endif
#ifdef USE_LITEETH
            usleep(SPI_TRANSACTION_TIME_US);
#endif
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

#ifdef USE_LITEPCIE
    flash_wait_done(conn, "flash_read_buffer_cmd", FLASH_READ, 32);
#endif
#ifdef USE_LITEETH
    usleep(SPI_TRANSACTION_TIME_US);
#endif

    for (i = 0; i < size; i += 4) {
        m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 0, 0);
        m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, 0);
        m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR,
                     SPI_CTRL_START | (32 * SPI_CTRL_LENGTH));

#ifdef USE_LITEPCIE
        flash_wait_done(conn, "flash_read_buffer_data", FLASH_READ, 32);
#endif
#ifdef USE_LITEETH
        usleep(SPI_TRANSACTION_TIME_US);
#endif

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
        if (flash_wait_while_busy(conn, base + i, "Erasing", FLASH_ERASE_TIMEOUT_US,
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

        if (flash_wait_while_busy(conn, base + i, "Waiting", FLASH_PROGRAM_TIMEOUT_US,
                                  progress_cb, opaque) != 0)
            return 1;

        flash_write_enable(conn);
        flash_write_buffer(conn, base + i, buf + i, prog_size);
        flash_write_disable(conn);

        if (flash_wait_while_busy(conn, base + i, "Writing", FLASH_PROGRAM_TIMEOUT_US,
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

#endif /* CSR_FLASH_BASE */
