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

#include "m2sdr_cdcm6208_spi.h"

//#define CDCM6208_SPI_WRITE_DEBUG
//#define CDCM6208_SPI_READ_DEBUG

/* Private Functions */

/* Public Functions */

void m2sdr_cdcm6208_spi_init(int fd) {
    /* Reset Through GPIO */
    //litepcie_writel(fd, CSR_CDCM6208_CONFIG_ADDR, 0b00);
    //usleep(1000);
    //litepcie_writel(fd, CSR_CDCM6208_CONFIG_ADDR, 0b11);
    //usleep(1000);

    /* Small delay. */
    usleep(1000);
}

void m2sdr_cdcm6208_spi_write(int fd, uint16_t reg, uint16_t dat) {
    litepcie_writel(fd, CSR_CDCM6208_SPI_MOSI_ADDR, ((reg & 0x7fff) << 16) | dat);
    litepcie_writel(fd, CSR_CDCM6208_SPI_CONTROL_ADDR, (32 << 8) | 1);
    while ((litepcie_readl(fd, CSR_CDCM6208_SPI_STATUS_ADDR) & 1) == 0);

#ifdef CDCM6208_SPI_WRITE_DEBUG
    printf("cdcm6208_spi_write_reg; reg:0x%04x dat:%04x\n", reg, dat);
#endif
}

uint8_t m2sdr_cdcm6208_spi_read(int fd, uint16_t reg) {
    litepcie_writel(fd, CSR_CDCM6208_SPI_MOSI_ADDR, ((reg & 0x7fff) | (1 << 15)) << 16);
    litepcie_writel(fd, CSR_CDCM6208_SPI_CONTROL_ADDR, (32 << 8) | 1);
    while ((litepcie_readl(fd, CSR_CDCM6208_SPI_STATUS_ADDR) & 1) == 0);
    return litepcie_readl(fd, CSR_CDCM6208_SPI_MISO_ADDR) & 0xffff;
}
