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

/* Helpers */
/*---------*/

/* m2sdr_ad9361_spi_init */
/*-----------------------*/

void m2sdr_ad9361_spi_init(void *conn, uint8_t reset) {
    if (reset) {
        /* The FPGA wrapper exposes the AD9361 reset line through the config
         * CSR, so an SPI reset here is really a short GPIO pulse. */
        m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b00);
        usleep(1000);
    }
    /* Re-enable the AD9361 interface and leave a small settle time before the
     * imported driver starts issuing register transactions. */
    m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b11);
    usleep(1000);

    /* Small delay. */
    usleep(1000);
}

/* m2sdr_ad9361_spi_xfer */
/*-----------------------*/

void m2sdr_ad9361_spi_xfer(void *conn, uint8_t len, uint8_t *mosi, uint8_t *miso) {
    (void)len;

    /* The AD9361 bridge always transfers 24 bits on this platform. The first
     * control byte encodes both the R/W bit and the high register address
     * bits, so we only need to distinguish read from write here. */
    bool is_write = (mosi[0] & 0x80) != 0;

    /* Write MOSI. */
    m2sdr_writel(conn, CSR_AD9361_SPI_MOSI_ADDR, mosi[0] << 16 | mosi[1] << 8 | mosi[2]);

    /* Start SPI. */
    m2sdr_writel(conn, CSR_AD9361_SPI_CONTROL_ADDR, 24*SPI_CONTROL_LENGTH | SPI_CONTROL_START);

    /* Wait done. */
#ifdef AD9361_SPI_WAIT_DONE
    /* Keep the helper synchronous so the caller sees a simple register-style
     * interface even though the FPGA block is command based. */
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

    /* Pack the AD9361 wire format: R/W bit + 15-bit register address + data. */
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

    /* A read reuses the same 24-bit frame but leaves the payload byte empty. */
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
