/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <unistd.h>

#include "liblitepcie.h"

#include "m2sdr_si5351_i2c.h"

void m2sdr_si5351_i2c_reset(int fd) {
    /* Reset Active. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 0);

    /* Reset Settings. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, 0);

    /* Flush RX FIFO. */
    while (litepcie_readl(fd, CSR_SI5351_I2C_MASTER_STATUS_ADDR) & (1 << CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET)) {
        litepcie_readl(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR);
    }
}

bool m2sdr_si5351_i2c_write(int fd, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len) {
    if (len != 1) {
        return false;
    }

    int status;
    int timeout;

    /* Reset I2C. */
    m2sdr_si5351_i2c_reset(fd);

    /* Configure Transaction: TX=2 bytes, RX=0. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, (0 << 8) | 2);

    /* Set Slave Address. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_ADDR_ADDR, slave_addr);

    /* Start Transaction. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 1);

    /* Wait TX Ready. */
    timeout = 100000;
    do {
        status = litepcie_readl(fd, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
        if (timeout-- <= 0) {
            return false;
        }
        usleep(1);
    } while (!(status & (1 << CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET)));

    /* Check NACK. */
    if (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET)) {
        return false;
    }

    /* Send Register Address and Data. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR, (addr << 8) | data[0]);

    /* Wait TX Ready. */
    timeout = 100000;
    do {
        status = litepcie_readl(fd, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
        if (timeout-- <= 0) {
            return false;
        }
        usleep(1);
    } while (!(status & (1 << CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET)));

    /* Check NACK. */
    if (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET)) {
        return false;
    }

    return true;
}

bool m2sdr_si5351_i2c_read(int fd, uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop) {
    if (len != 1) {
        return false;
    }

    int status;
    int timeout;

    /* Reset I2C. */
    m2sdr_si5351_i2c_reset(fd);

    /* Configure Transaction: TX=1 byte, RX=1 byte. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, (1 << 8) | 1);

    /* Set Slave Address. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_ADDR_ADDR, slave_addr);

    /* Start Transaction. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 1);

    /* Wait TX Ready. */
    timeout = 100000;
    do {
        status = litepcie_readl(fd, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
        if (timeout-- <= 0) {
            return false;
        }
        usleep(1);
    } while (!(status & (1 << CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET)));

    /* Check NACK. */
    if (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET)) {
        return false;
    }

    /* Send Register Address. */
    litepcie_writel(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR, addr);

    /* Wait RX Ready. */
    timeout = 100000;
    do {
        status = litepcie_readl(fd, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
        if (timeout-- <= 0) {
            return false;
        }
        usleep(1);
    } while (!(status & (1 << CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET)));

    /* Check NACK. */
    if (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET)) {
        return false;
    }

    /* Read Data. */
    *data = litepcie_readl(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR) & 0xFF;

    return true;
}

bool m2sdr_si5351_i2c_poll(int fd, uint8_t slave_addr) {
    uint8_t dummy;
    return m2sdr_si5351_i2c_read(fd, slave_addr, 0x00, &dummy, 1, true);
}

void m2sdr_si5351_i2c_config(int fd, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length) {
    int i;
    uint8_t data;

    /* Reset I2C Line */
    m2sdr_si5351_i2c_reset(fd);
    usleep(100);

    /* Wait for System Initialization (SYS_INIT = 0) */
    int timeout = 100;
    while (timeout--) {
        if (m2sdr_si5351_i2c_read(fd, i2c_addr, 0, &data, 1, false) && (data & 0x80) == 0) {
            break;
        }
        usleep(1000);
    }
    if (timeout <= 0) {
        return;
    }

    /* Disable all outputs */
    data = 0xFF;
    m2sdr_si5351_i2c_write(fd, i2c_addr, 3, &data, 1);

    /* Power down all output drivers */
    data = 0x80;
    for (i = 16; i <= 23; i++) {
        m2sdr_si5351_i2c_write(fd, i2c_addr, i, &data, 1);
    }

    /* Set interrupt masks */
    data = 0x00;
    m2sdr_si5351_i2c_write(fd, i2c_addr, 2, &data, 1);

    /* Configure SI5351 from provided register map */
    for (i = 0; i < i2c_length; i++) {
        uint8_t reg = i2c_config[i][0];
        uint8_t val = i2c_config[i][1];
        m2sdr_si5351_i2c_write(fd, i2c_addr, reg, &val, 1);
    }

    /* Apply PLLA and PLLB soft reset */
    data = 0xAC;
    m2sdr_si5351_i2c_write(fd, i2c_addr, 177, &data, 1);

    /* Wait for PLL lock */
    timeout = 100;
    while (timeout--) {
        if (m2sdr_si5351_i2c_read(fd, i2c_addr, 1, &data, 1, false) && (data & 0x60) == 0) {
            break;
        }
        usleep(1000);
    }
    if (timeout <= 0) {
        return;
    }

    /* Enable all outputs */
    data = 0x00;
    m2sdr_si5351_i2c_write(fd, i2c_addr, 3, &data, 1);
}
