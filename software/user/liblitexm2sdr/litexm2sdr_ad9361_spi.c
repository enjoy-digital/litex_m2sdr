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

#include "litexm2sdr_ad9361_spi.h"

#define AD9361_SPI_XFER_DEBUG

#define AD9361_SPI_WRITE_DEBUG
#define AD9361_SPI_READ_DEBUG

/* Private Functions */

/* Public Functions */

void ad9361_spi_init(int fd) {
    /* Set SPI Clk Freq */
    litepcie_writel(fd, CSR_AD9361_SPI_CLK_DIVIDER_ADDR, SYS_CLK_FREQ_MHZ/SPI_CLK_FREQ_MHZ);

    /* Configure AD361 for 4-Wire SPI */
    ad9361_spi_write(fd, SPI_AD9361_CS, 0, 0x3c);

    /* Small delay. */
    usleep(1000);
}

void ad9361_spi_xfer(int fd, uint8_t cs, uint8_t len, uint8_t *mosi, uint8_t *miso) {
    uint8_t i;

    /* Set Chip Select. */
    litepcie_writel(fd, CSR_AD9361_SPI_CS_ADDR, SPI_CS_MANUAL | (1 << cs));

    /* Do SPI Xfer. */
    i = 0;
    while (i < len) {
        if ((len - i) >= 4) {
            uint32_t _mosi = 0;
            uint32_t _miso = 0;
            _mosi |= mosi[i+0] << 24;
            _mosi |= mosi[i+1] << 16;
            _mosi |= mosi[i+2] <<  8;
            _mosi |= mosi[i+3] <<  0;
            litepcie_writel(fd, CSR_AD9361_SPI_MOSI_ADDR, _mosi);
            litepcie_writel(fd, CSR_AD9361_SPI_CONTROL_ADDR, 32*SPI_CONTROL_LENGTH | SPI_CONTROL_START);
            while ((litepcie_readl(fd, CSR_AD9361_SPI_STATUS_ADDR) & 0x1) != SPI_STATUS_DONE);
            if (miso != NULL) {
                _miso = litepcie_readl(fd, CSR_AD9361_SPI_MISO_ADDR);
                miso[i+0] = (_miso >> 24) & 0xff;
                miso[i+1] = (_miso >> 16) & 0xff;
                miso[i+2] = (_miso >>  8) & 0xff;
                miso[i+3] = (_miso >>  0) & 0xff;
            }
            i += 4;
        } else {
            litepcie_writel(fd, CSR_AD9361_SPI_MOSI_ADDR, mosi[i] << 24);
            litepcie_writel(fd, CSR_AD9361_SPI_CONTROL_ADDR, 8*SPI_CONTROL_LENGTH | SPI_CONTROL_START);
            while ((litepcie_readl(fd, CSR_AD9361_SPI_STATUS_ADDR) & 0x1) != SPI_STATUS_DONE);
            if (miso != NULL)
                miso[i] = litepcie_readl(fd, CSR_AD9361_SPI_MISO_ADDR) & 0xff;
            i += 1;
        }
    }

#ifdef AD9361_SPI_XFER_DEBUG
    printf("ad9361_spi_xfer; cs:%d len: %2d mosi:",cs, len);
    for (i=0; i<len; i++)
        printf(" 0x%02x", mosi[i]);
    printf("\n");
    if (miso != NULL) {
        for(i=0; i<32; i++)
            printf(" ");
        printf("miso:");
        for (i=0; i<len; i++)
            printf(" 0x%02x", miso[i]);
        printf("\n");
    }
#endif

    /* Clear Chip Select. */
    litepcie_writel(fd, CSR_AD9361_SPI_CS_ADDR,  SPI_CS_MANUAL | (0 << cs));
}

void ad9361_spi_write(int fd, uint8_t cs, uint16_t reg, uint8_t dat) {
    uint8_t mosi[3];
    uint8_t miso[3];

#ifdef AD9361_SPI_WRITE_DEBUG
    printf("ad9361_spi_write_reg; cs:%d reg:0x%04x dat:%02x\n",cs, reg, dat);
#endif

    /* Prepare Data. */
    mosi[0]  = (0 << 7);
    mosi[0] |= (reg >> 8) & 0x7f;
    mosi[1]  = (reg >> 0) & 0xff;
    mosi[2]  = dat;

    /* Do SPI Xfer. */
    ad9361_spi_xfer(fd, cs, 3, mosi, miso);
}

uint8_t ad9361_spi_read(int fd, uint8_t cs, uint16_t reg) {
    uint8_t dat;
    uint8_t mosi[3];
    uint8_t miso[3];

    /* Prepare Data. */
    mosi[0]  = (1 << 7);
    mosi[0] |= (reg >> 8) & 0x7f;
    mosi[1]  = (reg >> 0) & 0xff;
    mosi[2]  = 0x00;

    /* Do SPI Xfer. */
    ad9361_spi_xfer(fd, cs, 3, mosi, miso);

    /* Process Data. */
    dat = miso[2];

#ifdef AD9361_SPI_WRITE_DEBUG
    printf("ad9361_spi_read_reg; cs:%d reg:0x%04x dat:%02x\n", cs, reg, dat);
#endif

    return dat;
}
