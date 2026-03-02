/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Common - Signal handling, AD9361 platform stubs & shared RF init.
 *
 * Provides:
 *   - Signal handling (SIGINT/SIGTERM)
 *   - AD9361 platform abstraction layer (SPI, GPIO, delay stubs)
 *   - Common RF initialization code shared between streaming executables
 *   - AGC mode helpers
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#ifndef M2SDR_COMMON_H
#define M2SDR_COMMON_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "m2sdr_config.h"
#include "liblitepcie.h"
#include "libm2sdr.h"

/*
 * Signal Handling
 */
static volatile sig_atomic_t g_keep_running = 1;

static void m2sdr_signal_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static inline void m2sdr_install_signal_handlers(void) {
    signal(SIGINT,  m2sdr_signal_handler);
    signal(SIGTERM, m2sdr_signal_handler);
}

/*
 * AD9361 Platform Stubs
 */
#define AD9361_GPIO_RESET_PIN 0

struct ad9361_rf_phy *ad9361_phy;

static int g_spi_fd = -1;

int spi_write_then_read(struct spi_device *spi,
                        const unsigned char *txbuf, unsigned n_tx,
                        unsigned char *rxbuf, unsigned n_rx)
{
    (void)spi;
    if (n_tx == 2 && n_rx == 1) {
        rxbuf[0] = m2sdr_ad9361_spi_read((void *)(intptr_t)g_spi_fd, txbuf[0] << 8 | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        m2sdr_ad9361_spi_write((void *)(intptr_t)g_spi_fd, txbuf[0] << 8 | txbuf[1], txbuf[2]);
    } else {
        fprintf(stderr, "Unsupported SPI transfer n_tx=%d n_rx=%d\n", n_tx, n_rx);
        return -1;
    }
    return 0;
}

void udelay(unsigned long usecs) { usleep(usecs); }
void mdelay(unsigned long msecs) { usleep(msecs * 1000); }
unsigned long msleep_interruptible(unsigned int msecs) { usleep(msecs * 1000); return 0; }
bool gpio_is_valid(int number) { return (number == AD9361_GPIO_RESET_PIN); }
void gpio_set_value(unsigned gpio, int value) { (void)gpio; (void)value; }

/*
 * AGC Mode Helpers
 */
static const char *agc_mode_names[] __attribute__((unused)) = {"manual", "fast_attack", "slow_attack", "hybrid"};

static uint8_t __attribute__((unused)) parse_agc_mode(const char *str) {
    if (strcasecmp(str, "manual") == 0 || strcasecmp(str, "mgc") == 0)
        return RF_GAIN_MGC;
    if (strcasecmp(str, "fast_attack") == 0 || strcasecmp(str, "fast") == 0)
        return RF_GAIN_FASTATTACK_AGC;
    if (strcasecmp(str, "slow_attack") == 0 || strcasecmp(str, "slow") == 0)
        return RF_GAIN_SLOWATTACK_AGC;
    if (strcasecmp(str, "hybrid") == 0)
        return RF_GAIN_HYBRID_AGC;
    fprintf(stderr, "Unknown AGC mode '%s'. Valid: manual, fast_attack, slow_attack, hybrid\n", str);
    exit(1);
}

static inline const char *agc_mode_str(uint8_t mode) {
    return (mode <= RF_GAIN_HYBRID_AGC) ? agc_mode_names[mode] : "unknown";
}

/*
 * Common RF Initialization
 *
 * Performs the shared setup steps used by both RX and TX:
 *   - SI5351 clocking (if available)
 *   - AD9361 power-up and SPI init
 *   - 1T1R / 2T2R mode configuration
 *   - Reference clock and init params
 *   - 16-bit mode, synchronizer enable, loopback disable
 *
 * After calling this, the caller should do direction-specific setup
 * (RX freq/gain/AGC/FIR or TX freq/attenuation/FIR interpolation).
 *
 * The fd is stored in g_spi_fd for use by spi_write_then_read().
 */
static void m2sdr_rf_common_init(int fd, uint8_t num_channels)
{
    g_spi_fd = fd;

#ifdef CSR_SI5351_BASE
    printf("Initializing SI5351 clocking...\n");
    m2sdr_writel((void *)(intptr_t)fd, CSR_SI5351_CONTROL_ADDR,
        SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET));
    m2sdr_si5351_i2c_config((void *)(intptr_t)fd, SI5351_I2C_ADDR,
        si5351_xo_40m_config,
        sizeof(si5351_xo_40m_config) / sizeof(si5351_xo_40m_config[0]));
#endif

    printf("Powering up AD9361...\n");
    m2sdr_writel((void *)(intptr_t)fd, CSR_AD9361_CONFIG_ADDR, 0b11);

    printf("Initializing AD9361 SPI...\n");
    m2sdr_ad9361_spi_init((void *)(intptr_t)fd, 1);

    /* Configure 1T1R or 2T2R mode */
    if (num_channels == 2) {
        printf("Initializing AD9361 RFIC (2T2R mode)...\n");
        default_init_param.two_rx_two_tx_mode_enable     = 1;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
        default_init_param.two_t_two_r_timing_enable     = 1;
        m2sdr_writel((void *)(intptr_t)fd, CSR_AD9361_PHY_CONTROL_ADDR, 0);
    } else {
        printf("Initializing AD9361 RFIC (1T1R mode)...\n");
        default_init_param.two_rx_two_tx_mode_enable     = 0;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
        default_init_param.two_t_two_r_timing_enable     = 0;
        m2sdr_writel((void *)(intptr_t)fd, CSR_AD9361_PHY_CONTROL_ADDR, 1);
    }

    default_init_param.reference_clk_rate = 40000000;
    default_init_param.gpio_resetb        = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync          = -1;
    default_init_param.gpio_cal_sw1       = -1;
    default_init_param.gpio_cal_sw2       = -1;

    /* 16-bit mode (12-bit samples in 16-bit words) */
    m2sdr_writel((void *)(intptr_t)fd, CSR_AD9361_BITMODE_ADDR, 0);

    /* Enable synchronizer (disable bypass) - matches SoapySDR */
    m2sdr_writel((void *)(intptr_t)fd, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 0);

    /* Disable DMA loopback - matches SoapySDR */
    m2sdr_writel((void *)(intptr_t)fd, CSR_PCIE_DMA0_LOOPBACK_ENABLE_ADDR, 0);
}

#endif /* M2SDR_COMMON_H */
