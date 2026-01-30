/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Includes */
/*----------*/

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>

#include "libm2sdr.h"
#include "liblitepcie.h"

#include "m2sdr_ad9361_spi.h"

#include "etherbone.h"

/* Defines */
/*---------*/

#define AD9361_SPI_WAIT_DONE
//#define AD9361_SPI_WRITE_DEBUG
//#define AD9361_SPI_READ_DEBUG

/* m2sdr_ad9361_spi_init */
/*-----------------------*/

void m2sdr_ad9361_spi_init(void *conn, uint8_t reset) {
    if (reset) {
        /* Reset Through GPIO */
        m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b00);
        usleep(1000);
    }
    m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b11);
    usleep(1000);

    /* Small delay. */
    usleep(1000);
}

/* m2sdr_ad9361_spi_xfer */
/*-----------------------*/

void m2sdr_ad9361_spi_xfer(void *conn, uint8_t len, uint8_t *mosi, uint8_t *miso) {
    /* Check write. */
    bool is_write = (mosi[0] & 0x80) != 0;

    /* Write MOSI. */
    m2sdr_writel(conn, CSR_AD9361_SPI_MOSI_ADDR, mosi[0] << 16 | mosi[1] << 8 | mosi[2]);

    /* Start SPI. */
    m2sdr_writel(conn, CSR_AD9361_SPI_CONTROL_ADDR, 24*SPI_CONTROL_LENGTH | SPI_CONTROL_START);

    /* Wait done. */
#ifdef AD9361_SPI_WAIT_DONE
    while ((m2sdr_readl(conn, CSR_AD9361_SPI_STATUS_ADDR) & 0x1) != SPI_STATUS_DONE);
#endif

    /* Read MISO if read. */
    miso[2] = 0;
    if (!is_write) {
        miso[2] = m2sdr_readl(conn, CSR_AD9361_SPI_MISO_ADDR) & 0xff;
    }
}

/* m2sdr_ad9361_spi_write */
/*------------------------*/

void m2sdr_ad9361_spi_write(void *conn, uint16_t reg, uint8_t dat) {
    uint8_t mosi[3];
    uint8_t miso[3];

#ifdef AD9361_SPI_WRITE_DEBUG
    printf("ad9361_spi_write_reg; reg:0x%04x dat:%02x\n", reg, dat);
#endif

    /* Prepare Data. */
    mosi[0]  = (1 << 7);
    mosi[0] |= (reg >> 8) & 0x7f;
    mosi[1]  = (reg >> 0) & 0xff;
    mosi[2]  = dat;

    /* Do SPI Xfer. */
    m2sdr_ad9361_spi_xfer(conn, 3, mosi, miso);
}

/* m2sdr_ad9361_spi_read */
/*-----------------------*/

uint8_t m2sdr_ad9361_spi_read(void *conn, uint16_t reg) {
    uint8_t dat;
    uint8_t mosi[3];
    uint8_t miso[3];

    /* Prepare Data. */
    mosi[0]  = (0 << 7);
    mosi[0] |= (reg >> 8) & 0x7f;
    mosi[1]  = (reg >> 0) & 0xff;
    mosi[2]  = 0x00;

    /* Do SPI Xfer. */
    m2sdr_ad9361_spi_xfer(conn, 3, mosi, miso);

    /* Process Data. */
    dat = miso[2];

#ifdef AD9361_SPI_READ_DEBUG
    printf("ad9361_spi_read_reg; reg:0x%04x dat:%02x\n", reg, dat);
#endif

    return dat;
}
