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

#include "m2sdr_si5351_i2c.h"

#include "etherbone.h"

/* Functions */
/*-----------*/

/* The LiteI2C controller is used in a polling mode here so the same helper can
 * be shared by PCIe and Etherbone builds without any interrupt dependency. */

/* m2sdr_si5351_i2c_reset */
/*------------------------*/

void m2sdr_si5351_i2c_reset(void *conn) {
    /* Reset Active. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 0);

    /* Reset Settings. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, 0);

    /* Always drain stale RX bytes so the next transaction starts from a known
     * empty state, which matters when switching between reads and writes. */
    while (m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_STATUS_ADDR) & (1 << CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET)) {
        m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_RXTX_ADDR);
    }
}

/* m2sdr_si5351_i2c_write */
/*------------------------*/

bool m2sdr_si5351_i2c_write(void *conn, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len) {
    if (len != 1) {
        /* The current LiteI2C helper is intentionally narrow: the config path
         * only needs single-register writes. */
        return false;
    }

    int status;
    int timeout;

    /* Reset I2C. */
    m2sdr_si5351_i2c_reset(conn);

    /* One byte for the target register, one byte for the value. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, (0 << 8) | 2);

    /* Set Slave Address. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_ADDR_ADDR, slave_addr);

    /* Start Transaction. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 1);

    /* Wait TX Ready. */
    timeout = 100000;
    do {
        status = m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
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
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_RXTX_ADDR, (addr << 8) | data[0]);

    /* Wait TX Ready. */
    timeout = 100000;
    do {
        status = m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
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

/* m2sdr_si5351_i2c_read */
/*-----------------------*/

bool m2sdr_si5351_i2c_read(void *conn, uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop) {
    (void)send_stop;

    if (len != 1) {
        return false;
    }

    int status;
    int timeout;

    /* Reset I2C. */
    m2sdr_si5351_i2c_reset(conn);

    /* One address byte is written first, then one data byte is read back. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, (1 << 8) | 1);

    /* Set Slave Address. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_ADDR_ADDR, slave_addr);

    /* Start Transaction. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 1);

    /* Wait TX Ready. */
    timeout = 100000;
    do {
        status = m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
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
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_RXTX_ADDR, addr);

    /* Wait RX Ready. */
    timeout = 100000;
    do {
        status = m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
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
    *data = m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_RXTX_ADDR) & 0xFF;

    return true;
}

/* m2sdr_si5351_i2c_poll */
/*-----------------------*/

bool m2sdr_si5351_i2c_poll(void *conn, uint8_t slave_addr) {
    uint8_t dummy;
    /* Poll by attempting a harmless register read; success implies both bus and
     * target are responsive. */
    return m2sdr_si5351_i2c_read(conn, slave_addr, 0x00, &dummy, 1, true);
}

/* m2sdr_si5351_i2c_check_litei2c */
/*--------------------------------*/

bool m2sdr_si5351_i2c_check_litei2c(void *conn) {
    return m2sdr_readl(conn, CSR_SI5351_BASE) != 0x5;
}

/* m2sdr_si5351_i2c_config */
/*-------------------------*/

void m2sdr_si5351_i2c_config(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length) {
    int i;
    uint8_t data;

    /* Check for LiteI2C. */
    if (m2sdr_si5351_i2c_check_litei2c(conn) == false) {
        printf("Old gateware detected: SI5351 Software I2C access is not supported. Please update gateware.\n");
        return;
    }

    /* Reset I2C Line */
    m2sdr_si5351_i2c_reset(conn);
    usleep(100);

    /* Wait for the SI5351 internal SYS_INIT flag to clear before pushing the
     * full clock tree configuration. */
    int timeout = 100;
    while (timeout--) {
        if (m2sdr_si5351_i2c_read(conn, i2c_addr, 0, &data, 1, false) && (data & 0x80) == 0) {
            break;
        }
        usleep(1000);
    }
    if (timeout <= 0) {
        return;
    }

    /* Disable all outputs */
    data = 0xFF;
    m2sdr_si5351_i2c_write(conn, i2c_addr, 3, &data, 1);

    /* Power down all output drivers */
    data = 0x80;
    for (i = 16; i <= 23; i++) {
        m2sdr_si5351_i2c_write(conn, i2c_addr, i, &data, 1);
    }

    /* Set interrupt masks */
    data = 0x00;
    m2sdr_si5351_i2c_write(conn, i2c_addr, 2, &data, 1);

    /* Apply the selected precomputed register table verbatim. */
    for (i = 0; i < i2c_length; i++) {
        uint8_t reg = i2c_config[i][0];
        uint8_t val = i2c_config[i][1];
        m2sdr_si5351_i2c_write(conn, i2c_addr, reg, &val, 1);
    }

    /* Apply PLLA and PLLB soft reset */
    data = 0xAC;
    m2sdr_si5351_i2c_write(conn, i2c_addr, 177, &data, 1);

    /* Do not re-enable outputs until both PLLs report locked. */
    timeout = 100;
    while (timeout--) {
        if (m2sdr_si5351_i2c_read(conn, i2c_addr, 1, &data, 1, false) && (data & 0x60) == 0) {
            break;
        }
        usleep(1000);
    }
    if (timeout <= 0) {
        return;
    }

    /* Enable all outputs */
    data = 0x00;
    m2sdr_si5351_i2c_write(conn, i2c_addr, 3, &data, 1);
}
