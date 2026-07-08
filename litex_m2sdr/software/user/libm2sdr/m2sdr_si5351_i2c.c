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

/* Defines */
/*---------*/

#define SI5351_I2C_DRAIN_LIMIT 256
#define SI5351_STATUS_SYS_INIT 0x80
#define SI5351_STATUS_LOL_B    0x40
#define SI5351_CONFIG_TIMEOUT_MS 100

/* PLLB feedback Multisynth register block (AN619 registers 34..41) and the
 * valid a + b/c feedback multiplier range. */
#define SI5351_PLLB_FB_BASE_REG  0x22
#define SI5351_PLLB_FB_NUM_REGS  8
#define SI5351_PLL_FB_MULT_MIN   15.0
#define SI5351_PLL_FB_MULT_MAX   90.0
#define SI5351_PLL_FB_TRIM_DENOM 1000000

/* Functions */
/*-----------*/

/* The LiteI2C controller is used in a polling mode here so the same helper can
 * be shared by PCIe and Etherbone builds without any interrupt dependency. */

static bool m2sdr_si5351_bus_ok(void *conn)
{
    if (m2sdr_legacy_handle_is_fd(conn))
        return true;

    return eb_get_last_error((struct eb_connection *)conn) == EB_ERR_OK;
}

/* m2sdr_si5351_i2c_reset */
/*------------------------*/

void m2sdr_si5351_i2c_reset(void *conn) {
    unsigned drain_count = 0;

    /* Reset Active. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_ACTIVE_ADDR, 0);

    /* Reset Settings. */
    m2sdr_writel(conn, CSR_SI5351_I2C_MASTER_SETTINGS_ADDR, 0);

    /* Always drain stale RX bytes so the next transaction starts from a known
     * empty state, which matters when switching between reads and writes. */
    while (drain_count++ < SI5351_I2C_DRAIN_LIMIT) {
        uint32_t status = m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_STATUS_ADDR);

        if (!m2sdr_si5351_bus_ok(conn))
            break;
        if (!(status & (1 << CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET)))
            break;
        m2sdr_readl(conn, CSR_SI5351_I2C_MASTER_RXTX_ADDR);
        if (!m2sdr_si5351_bus_ok(conn))
            break;
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
        if (!m2sdr_si5351_bus_ok(conn)) {
            return false;
        }
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
        if (!m2sdr_si5351_bus_ok(conn)) {
            return false;
        }
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
        if (!m2sdr_si5351_bus_ok(conn)) {
            return false;
        }
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
        if (!m2sdr_si5351_bus_ok(conn)) {
            return false;
        }
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
    if (!m2sdr_si5351_bus_ok(conn)) {
        return false;
    }

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
    uint32_t value = m2sdr_readl(conn, CSR_SI5351_BASE);

    if (!m2sdr_si5351_bus_ok(conn)) {
        return false;
    }

    return value != 0x5;
}

/* m2sdr_si5351_i2c_config */
/*-------------------------*/

bool m2sdr_si5351_i2c_config_checked(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length) {
    int i;
    uint8_t data;

    /* Check for LiteI2C. */
    if (m2sdr_si5351_i2c_check_litei2c(conn) == false) {
        if (!m2sdr_si5351_bus_ok(conn)) {
            return false;
        }
        printf("Old gateware detected: SI5351 Software I2C access is not supported. Please update gateware.\n");
        return true;
    }

    /* Reset I2C Line */
    m2sdr_si5351_i2c_reset(conn);
    if (!m2sdr_si5351_bus_ok(conn)) {
        fprintf(stderr, "SI5351 I2C reset failed during config.\n");
        return false;
    }
    usleep(100);

    /* Wait for the SI5351 internal SYS_INIT flag to clear before pushing the
     * full clock tree configuration. */
    int timeout = SI5351_CONFIG_TIMEOUT_MS;
    while (timeout--) {
        if (m2sdr_si5351_i2c_read(conn, i2c_addr, 0, &data, 1, false) && (data & 0x80) == 0) {
            break;
        }
        if (!m2sdr_si5351_bus_ok(conn)) {
            fprintf(stderr, "SI5351 SYS_INIT poll failed during config.\n");
            return false;
        }
        usleep(1000);
    }
    if (timeout <= 0) {
        fprintf(stderr, "SI5351 SYS_INIT did not clear during config (status 0x%02x).\n", data);
        return false;
    }

    /* Disable all outputs */
    data = 0xFF;
    if (!m2sdr_si5351_i2c_write(conn, i2c_addr, 3, &data, 1)) {
        fprintf(stderr, "SI5351 failed to disable outputs during config.\n");
        return false;
    }

    /* Power down all output drivers */
    data = 0x80;
    for (i = 16; i <= 23; i++) {
        if (!m2sdr_si5351_i2c_write(conn, i2c_addr, i, &data, 1)) {
            fprintf(stderr, "SI5351 failed to power down output %d during config.\n", i - 16);
            return false;
        }
    }

    /* Set interrupt masks */
    data = 0x00;
    if (!m2sdr_si5351_i2c_write(conn, i2c_addr, 2, &data, 1)) {
        fprintf(stderr, "SI5351 failed to set interrupt masks during config.\n");
        return false;
    }

    /* Apply the selected precomputed register table verbatim. */
    for (i = 0; i < i2c_length; i++) {
        uint8_t reg = i2c_config[i][0];
        uint8_t val = i2c_config[i][1];
        if (!m2sdr_si5351_i2c_write(conn, i2c_addr, reg, &val, 1)) {
            fprintf(stderr, "SI5351 config write failed at table index %d, reg 0x%02x.\n", i, reg);
            return false;
        }
    }

    /* Apply PLLA and PLLB soft reset */
    data = 0xAC;
    if (!m2sdr_si5351_i2c_write(conn, i2c_addr, 177, &data, 1)) {
        fprintf(stderr, "SI5351 failed to reset PLLs during config.\n");
        return false;
    }

    /* The shipped clock tables drive all active outputs from PLLB. PLLA may
     * remain unlocked in normal internal-XO operation, so only gate on PLLB. */
    timeout = SI5351_CONFIG_TIMEOUT_MS;
    while (timeout--) {
        if (m2sdr_si5351_i2c_read(conn, i2c_addr, 0, &data, 1, false) &&
            (data & (SI5351_STATUS_SYS_INIT | SI5351_STATUS_LOL_B)) == 0) {
            break;
        }
        if (!m2sdr_si5351_bus_ok(conn)) {
            fprintf(stderr, "SI5351 PLLB lock poll failed during config.\n");
            return false;
        }
        usleep(1000);
    }
    if (timeout <= 0) {
        fprintf(stderr, "SI5351 PLLB did not lock during config (status 0x%02x).\n", data);
        return false;
    }

    /* Enable all outputs */
    data = 0x00;
    if (!m2sdr_si5351_i2c_write(conn, i2c_addr, 3, &data, 1)) {
        fprintf(stderr, "SI5351 failed to enable outputs during config.\n");
        return false;
    }

    return true;
}

void m2sdr_si5351_i2c_config(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length) {
    (void)m2sdr_si5351_i2c_config_checked(conn, i2c_addr, i2c_config, i2c_length);
}

/* m2sdr_si5351_i2c_trim_pllb_ppm */
/*--------------------------------*/

bool m2sdr_si5351_i2c_trim_pllb_ppm(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length, double ppm) {
    uint8_t  regs[SI5351_PLLB_FB_NUM_REGS];
    size_t   found = 0;
    uint32_t p1, p2, p3;
    uint32_t fb_int;
    uint64_t fb_frac;
    double   multiplier;
    size_t   i;

    /* Recover the nominal PLLB feedback registers from the config table so
     * the correction is always relative to the shipped clock tree, never to a
     * previously trimmed state. */
    for (i = 0; i < i2c_length; i++) {
        uint8_t reg = i2c_config[i][0];
        if (reg >= SI5351_PLLB_FB_BASE_REG &&
            reg <  SI5351_PLLB_FB_BASE_REG + SI5351_PLLB_FB_NUM_REGS) {
            regs[reg - SI5351_PLLB_FB_BASE_REG] = i2c_config[i][1];
            found++;
        }
    }
    if (found != SI5351_PLLB_FB_NUM_REGS)
        return false;

    /* Decode the P1/P2/P3 encoding back to the a + b/c feedback multiplier
     * (AN619: P1 = 128*a + floor(128*b/c) - 512, P2 = 128*b - c*floor(128*b/c),
     * P3 = c). */
    p3 = ((uint32_t)(regs[5] & 0xF0) << 12) | ((uint32_t)regs[0] << 8) | regs[1];
    p1 = ((uint32_t)(regs[2] & 0x03) << 16) | ((uint32_t)regs[3] << 8) | regs[4];
    p2 = ((uint32_t)(regs[5] & 0x0F) << 16) | ((uint32_t)regs[6] << 8) | regs[7];
    if (p3 == 0)
        return false;
    multiplier  = (double)((p1 + 512) >> 7);
    multiplier += (double)(((uint64_t)((p1 + 512) & 0x7F) * p3) + p2) / (128.0 * (double)p3);

    /* Scale the multiplier so all outputs land back on their nominal
     * frequencies: a reference running fast by +ppm needs the feedback
     * reduced by the same ratio. */
    multiplier /= 1.0 + ppm * 1e-6;
    if (multiplier < SI5351_PLL_FB_MULT_MIN || multiplier > SI5351_PLL_FB_MULT_MAX)
        return false;

    /* Re-encode with a fixed 1e6 denominator, giving a ~0.03 ppm trim
     * resolution on the shipped clock trees. */
    fb_int  = (uint32_t)multiplier;
    fb_frac = (uint64_t)((multiplier - (double)fb_int) * SI5351_PLL_FB_TRIM_DENOM + 0.5);
    if (fb_frac >= SI5351_PLL_FB_TRIM_DENOM) {
        fb_int += 1;
        fb_frac = 0;
    }
    p1 = 128 * fb_int + (uint32_t)((128 * fb_frac) / SI5351_PLL_FB_TRIM_DENOM) - 512;
    p2 = (uint32_t)(128 * fb_frac - SI5351_PLL_FB_TRIM_DENOM * ((128 * fb_frac) / SI5351_PLL_FB_TRIM_DENOM));
    p3 = SI5351_PLL_FB_TRIM_DENOM;

    regs[0] = (p3 >> 8)  & 0xFF;
    regs[1] = (p3 >> 0)  & 0xFF;
    regs[2] = (p1 >> 16) & 0x03;
    regs[3] = (p1 >> 8)  & 0xFF;
    regs[4] = (p1 >> 0)  & 0xFF;
    regs[5] = ((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F);
    regs[6] = (p2 >> 8)  & 0xFF;
    regs[7] = (p2 >> 0)  & 0xFF;

    /* Fractional feedback updates take effect without a PLL soft reset, so
     * the trim pulls the running clock tree smoothly. */
    for (i = 0; i < SI5351_PLLB_FB_NUM_REGS; i++) {
        if (!m2sdr_si5351_i2c_write(conn, i2c_addr, SI5351_PLLB_FB_BASE_REG + i, &regs[i], 1))
            return false;
    }

    return true;
}
