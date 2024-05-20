/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <unistd.h>

#include "liblitepcie.h"

#include "m2sdr_ad9361_spi.h"

//#define AD9361_SPI_WRITE_DEBUG
//#define AD9361_SPI_READ_DEBUG

/* Private Functions */

/* Public Functions */

void m2sdr_ad9361_spi_init(int fd) {
    /* Reset Through GPIO */
    litepcie_writel(fd, CSR_MAIN_AD9361_ENABLE_ADDR, 0);
    usleep(1000);
    litepcie_writel(fd, CSR_MAIN_AD9361_ENABLE_ADDR, 1);
    usleep(1000);

    /* Small delay. */
    usleep(1000);
}

void m2sdr_ad9361_spi_xfer(int fd, uint8_t len, uint8_t *mosi, uint8_t *miso) {
    litepcie_writel(fd, CSR_AD9361_SPI_MOSI_ADDR, mosi[0] << 16 | mosi[1] << 8 | mosi[2]);
    litepcie_writel(fd, CSR_AD9361_SPI_CTRL_ADDR, 24*SPI_CONTROL_LENGTH | SPI_CONTROL_START);
    while ((litepcie_readl(fd, CSR_AD9361_SPI_STATUS_ADDR) & 0x1) != SPI_STATUS_DONE);
    miso[2] = litepcie_readl(fd, CSR_AD9361_SPI_MISO_ADDR) & 0xff;
}

void m2sdr_ad9361_spi_write(int fd, uint16_t reg, uint8_t dat) {
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
    m2sdr_ad9361_spi_xfer(fd, 3, mosi, miso);
}

uint8_t m2sdr_ad9361_spi_read(int fd, uint16_t reg) {
    uint8_t dat;
    uint8_t mosi[3];
    uint8_t miso[3];

    /* Prepare Data. */
    mosi[0]  = (0 << 7);
    mosi[0] |= (reg >> 8) & 0x7f;
    mosi[1]  = (reg >> 0) & 0xff;
    mosi[2]  = 0x00;

    /* Do SPI Xfer. */
    m2sdr_ad9361_spi_xfer(fd, 3, mosi, miso);

    /* Process Data. */
    dat = miso[2];

#ifdef AD9361_SPI_WRITE_DEBUG
    printf("ad9361_spi_read_reg; reg:0x%04x dat:%02x\n", reg, dat);
#endif

    return dat;
}
