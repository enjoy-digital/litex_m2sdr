/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "liblitepcie.h"
#include "m2sdr_si5351_i2c.h"

#define I2C_DEBUG 0              /* Set to 1 for verbose debug logging in read/write/config */

#define I2C_TIMEOUT_MS 100       /* Timeout in ms for wait operations */

/* I2C Core Functions */

static uint32_t i2c_read_reg(int fd, uint32_t addr) {
    return litepcie_readl(fd, addr);
}

static void i2c_write_reg(int fd, uint32_t addr, uint32_t value) {
    litepcie_writel(fd, addr, value);
}

static int i2c_get_status(int fd) {
    return i2c_read_reg(fd, CSR_SI5351_I2C_MASTER_STATUS_ADDR);
}

static int i2c_wait_for_flag(int fd, uint32_t flag_offset, int timeout_ms) {
    int64_t start = get_time_ms();
    while (get_time_ms() - start < timeout_ms) {
        int status = i2c_get_status(fd);
        if (status & (1 << flag_offset)) {
            return status;
        }
        usleep(100);
    }
    return -1;  /* Timeout */
}

static void i2c_flush_rx(int fd) {
    while (i2c_get_status(fd) & (1 << CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET)) {
        i2c_read_reg(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR);
    }
}

/* Public Functions */

void m2sdr_si5351_i2c_reset(int fd) {
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 0);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, 0);
    i2c_flush_rx(fd);
}

bool m2sdr_si5351_i2c_write(int fd, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len) {
    if (len != 1) {
        fprintf(stderr, "Multi-byte write not supported\n");
        return false;
    }

    m2sdr_si5351_i2c_reset(fd);

    // Configure transaction: TX=2 bytes, RX=0
    uint32_t settings = (2 << CSR_SI5351_I2C_MASTER_SETTINGS_LEN_TX_OFFSET) | (0 << CSR_SI5351_I2C_MASTER_SETTINGS_LEN_RX_OFFSET);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, settings);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_ADDR_ADDR, slave_addr);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 1);

    // Send register address
    int status = i2c_wait_for_flag(fd, CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET, I2C_TIMEOUT_MS);
    if (status < 0 || (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET))) {
#if I2C_DEBUG
        fprintf(stderr, "NACK on slave addr\n");
#endif
        return false;
    }
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR, addr);

    // Send value
    status = i2c_wait_for_flag(fd, CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET, I2C_TIMEOUT_MS);
    if (status < 0 || (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET))) {
#if I2C_DEBUG
        fprintf(stderr, "NACK on register addr\n");
#endif
        return false;
    }
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR, data[0]);

    // Final check for completion
    status = i2c_wait_for_flag(fd, CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET, I2C_TIMEOUT_MS);
    if (status < 0 || (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET))) {
#if I2C_DEBUG
        fprintf(stderr, "NACK on data\n");
#endif
        return false;
    }

#if I2C_DEBUG
    fprintf(stderr, "I2C write succeeded\n");
#endif
    return true;
}

bool m2sdr_si5351_i2c_read(int fd, uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop) {
    if (len != 1) {
        fprintf(stderr, "Multi-byte read not supported\n");
        return false;
    }

    m2sdr_si5351_i2c_reset(fd);

    // Configure transaction: 1 byte TX (register address), 1 byte RX
    uint32_t settings = (1 << CSR_SI5351_I2C_MASTER_SETTINGS_LEN_TX_OFFSET) | (1 << CSR_SI5351_I2C_MASTER_SETTINGS_LEN_RX_OFFSET);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, settings);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_ADDR_ADDR, slave_addr);
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 1);

    // Send register address
    int status = i2c_wait_for_flag(fd, CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET, I2C_TIMEOUT_MS);
    if (status < 0 || (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET))) {
#if I2C_DEBUG
        fprintf(stderr, "NACK on slave addr\n");
#endif
        return false;
    }
    i2c_write_reg(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR, addr);

    // Read data byte
    status = i2c_wait_for_flag(fd, CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET, I2C_TIMEOUT_MS);
    if (status < 0 || (status & (1 << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET))) {
#if I2C_DEBUG
        fprintf(stderr, "NACK on read\n");
#endif
        return false;
    }
    data[0] = i2c_read_reg(fd, CSR_SI5351_I2C_MASTER_RXTX_ADDR) & 0xFF;

#if I2C_DEBUG
    fprintf(stderr, "I2C read succeeded\n");
#endif
    return true;
}

bool m2sdr_si5351_i2c_poll(int fd, uint8_t slave_addr) {
    uint8_t dummy;
    return m2sdr_si5351_i2c_read(fd, slave_addr, 0x00, &dummy, 1, true);
}

void m2sdr_si5351_i2c_config(int fd, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length) {
    int i;
    uint8_t data;
    bool success;

    /* Reset I2C Line */
    m2sdr_si5351_i2c_reset(fd);
    usleep(100);

    /* Wait for System Initialization (SYS_INIT = 0) */
    int timeout = 100;  /* 100 attempts */
    while (timeout--) {
        success = m2sdr_si5351_i2c_read(fd, i2c_addr, 0, &data, 1, false);  // Use repeated start
        if (success && (data & 0x80) == 0) {
            break;
        }
        usleep(1000);  /* Wait 1 ms */
    }
    if (timeout <= 0) {
        fprintf(stderr, "SI5351 system initialization timeout\n");
        return;
    }

    /* Disable all outputs */
    data = 0xFF;
    if (!m2sdr_si5351_i2c_write(fd, i2c_addr, 3, &data, 1)) {
        fprintf(stderr, "Failed to disable SI5351 outputs (reg 0x03)\n");
    }

    /* Power down all output drivers */
    data = 0x80;
    for (i = 16; i <= 23; i++) {
        if (!m2sdr_si5351_i2c_write(fd, i2c_addr, i, &data, 1)) {
            fprintf(stderr, "Failed to power down SI5351 output (reg 0x%02X)\n", i);
        }
    }

    /* Set interrupt masks */
    data = 0x00;
    if (!m2sdr_si5351_i2c_write(fd, i2c_addr, 2, &data, 1)) {
        fprintf(stderr, "Failed to set SI5351 interrupt masks (reg 0x02)\n");
    }

    /* Configure SI5351 from provided register map */
    for (i = 0; i < i2c_length; i++) {
        uint8_t reg = i2c_config[i][0];
        uint8_t val = i2c_config[i][1];
        if (!m2sdr_si5351_i2c_write(fd, i2c_addr, reg, &val, 1)) {
            fprintf(stderr, "Failed to write to SI5351 at register 0x%02X\n", reg);
        }
    }

    /* Apply PLLA and PLLB soft reset */
    data = 0xAC;
    if (!m2sdr_si5351_i2c_write(fd, i2c_addr, 177, &data, 1)) {
        fprintf(stderr, "Failed to apply SI5351 PLL soft reset (reg 0xB1)\n");
    }

    /* Wait for PLL lock */
    timeout = 100;  /* 100 attempts */
    while (timeout--) {
        success = m2sdr_si5351_i2c_read(fd, i2c_addr, 1, &data, 1, false);  // Use repeated start
        if (success && (data & 0x60) == 0) {  /* LOL_B (bit 7=0), LOL_A (bit 5=0) */
            break;
        }
        usleep(1000);  /* Wait 1 ms */
    }
    if (timeout <= 0) {
        fprintf(stderr, "SI5351 PLL lock timeout\n");
    }

    /* Enable all outputs */
    data = 0x00;
    if (!m2sdr_si5351_i2c_write(fd, i2c_addr, 3, &data, 1)) {
        fprintf(stderr, "Failed to enable SI5351 outputs (reg 0x03)\n");
    }
}
