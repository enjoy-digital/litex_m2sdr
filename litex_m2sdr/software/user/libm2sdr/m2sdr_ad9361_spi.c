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

#define AD9361_SPI_TIMEOUT_PCIE_US 100000
#define AD9361_SPI_TIMEOUT_ETH_US   10000
#define AD9361_RESET_PULSE_US 1000

/* Post-RESETB bring-up: poll the product-ID until the AD9361 answers, so bring-up proceeds as soon
 * as the chip is ready. Bounded by _MAX_US so a non-answering chip still returns and the caller's
 * ad9361_init() reports the failure. */
#define AD9361_RESET_SETTLE_MIN_US  1000    /* quiet time before the first SPI probe   */
#define AD9361_RESET_SETTLE_MAX_US  100000  /* upper bound on the whole settle wait     */
#define AD9361_RESET_POLL_US        1000    /* product-ID poll interval                 */
#define AD9361_REG_PRODUCT_ID       0x037
#define AD9361_PRODUCT_ID_MASK      0xF8    /* REG_PRODUCT_ID mask (ad9361.h)           */
#define AD9361_PRODUCT_ID_9361      0x08    /* PRODUCT_ID_9361     (ad9361.h)           */

/* Helpers */
/*---------*/

static int m2sdr_ad9361_spi_timeout_us(void *conn)
{
    return m2sdr_legacy_handle_is_fd(conn) ?
        AD9361_SPI_TIMEOUT_PCIE_US : AD9361_SPI_TIMEOUT_ETH_US;
}

static bool m2sdr_ad9361_bus_ok(void *conn)
{
    if (m2sdr_legacy_handle_is_fd(conn))
        return true;

    return eb_get_last_error((struct eb_connection *)conn) == EB_ERR_OK;
}

/* m2sdr_ad9361_spi_init */
/*-----------------------*/

void m2sdr_ad9361_spi_init(void *conn, uint8_t reset) {
    if (reset) {
        /* The FPGA wrapper exposes the AD9361 reset line through the config
         * CSR, so an SPI reset here is really a short GPIO pulse. */
        m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b00);
        usleep(AD9361_RESET_PULSE_US);
    }
    /* Re-enable the AD9361 interface, then wait for the chip to actually answer on
     * SPI before the imported driver starts issuing register transactions. After
     * RESETB deasserts (REFCLK already present from the SI5351) the part needs a
     * short internal reset; polling the product-ID lets bring-up proceed as soon as
     * the chip is ready. A chip that never answers still returns within
     * AD9361_RESET_SETTLE_MAX_US so ad9361_init() surfaces the real error. */
    m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b11);
    usleep(AD9361_RESET_SETTLE_MIN_US);
    for (int waited_us = AD9361_RESET_SETTLE_MIN_US;
         waited_us < AD9361_RESET_SETTLE_MAX_US;
         waited_us += AD9361_RESET_POLL_US) {
        uint8_t id = 0;
        if (m2sdr_ad9361_spi_read_checked(conn, AD9361_REG_PRODUCT_ID, &id) &&
            (id & AD9361_PRODUCT_ID_MASK) == AD9361_PRODUCT_ID_9361)
            return;
        usleep(AD9361_RESET_POLL_US);
    }
}

/* m2sdr_ad9361_spi_xfer */
/*-----------------------*/

bool m2sdr_ad9361_spi_xfer_checked(void *conn, uint8_t len, uint8_t *mosi, uint8_t *miso) {
    (void)len;

    /* The AD9361 bridge always transfers 24 bits on this platform. The first
     * control byte encodes both the R/W bit and the high register address
     * bits, so we only need to distinguish read from write here. */
    bool is_write = (mosi[0] & 0x80) != 0;
    bool done = false;

    /* Write MOSI. */
    m2sdr_writel(conn, CSR_AD9361_SPI_MOSI_ADDR, mosi[0] << 16 | mosi[1] << 8 | mosi[2]);
    if (!m2sdr_ad9361_bus_ok(conn))
        return false;

    /* Start SPI. */
    m2sdr_writel(conn, CSR_AD9361_SPI_CONTROL_ADDR, 24*SPI_CONTROL_LENGTH | SPI_CONTROL_START);
    if (!m2sdr_ad9361_bus_ok(conn))
        return false;

    /* Wait done. */
#ifdef AD9361_SPI_WAIT_DONE
    /* Keep the helper synchronous so the caller sees a simple register-style
     * interface even though the FPGA block is command based. */
    for (int timeout = m2sdr_ad9361_spi_timeout_us(conn); timeout > 0; timeout--) {
        if ((m2sdr_readl(conn, CSR_AD9361_SPI_STATUS_ADDR) & 0x1) == SPI_STATUS_DONE) {
            done = true;
            break;
        }
        if (!m2sdr_ad9361_bus_ok(conn))
            return false;
        usleep(1);
    }
#else
    done = true;
#endif
    if (!done)
        return false;

    /* Read MISO if read. */
    miso[2] = 0;
    if (!is_write) {
        miso[2] = m2sdr_readl(conn, CSR_AD9361_SPI_MISO_ADDR) & 0xff;
        if (!m2sdr_ad9361_bus_ok(conn))
            return false;
    }

    return true;
}

void m2sdr_ad9361_spi_xfer(void *conn, uint8_t len, uint8_t *mosi, uint8_t *miso) {
    (void)m2sdr_ad9361_spi_xfer_checked(conn, len, mosi, miso);
}

/* m2sdr_ad9361_spi_write */
/*------------------------*/

void m2sdr_ad9361_spi_write(void *conn, uint16_t reg, uint8_t dat) {
    (void)m2sdr_ad9361_spi_write_checked(conn, reg, dat);
}

bool m2sdr_ad9361_spi_write_checked(void *conn, uint16_t reg, uint8_t dat) {
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
    return m2sdr_ad9361_spi_xfer_checked(conn, 3, mosi, miso);
}

/* m2sdr_ad9361_spi_read */
/*-----------------------*/

uint8_t m2sdr_ad9361_spi_read(void *conn, uint16_t reg) {
    uint8_t dat = 0;

    (void)m2sdr_ad9361_spi_read_checked(conn, reg, &dat);
    return dat;
}

bool m2sdr_ad9361_spi_read_checked(void *conn, uint16_t reg, uint8_t *dat) {
    uint8_t mosi[3];
    uint8_t miso[3];

    if (!dat)
        return false;

    /* A read reuses the same 24-bit frame but leaves the payload byte empty. */
    mosi[0]  = (0 << 7);
    mosi[0] |= (reg >> 8) & 0x7f;
    mosi[1]  = (reg >> 0) & 0xff;
    mosi[2]  = 0x00;

    /* Do SPI Xfer. */
    if (!m2sdr_ad9361_spi_xfer_checked(conn, 3, mosi, miso))
        return false;

    /* Process Data. */
    *dat = miso[2];

#ifdef AD9361_SPI_READ_DEBUG
    printf("ad9361_spi_read_reg; reg:0x%04x dat:%02x\n", reg, *dat);
#endif

    return true;
}
