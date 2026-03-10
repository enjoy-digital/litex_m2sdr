/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

/* Includes */
/*----------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "ad9361.h"
#include "ad9361_api.h"
#include "csr.h"
#include "m2sdr_ad9361_spi.h"
#include "m2sdr_config.h"
#include "m2sdr_internal.h"
#include "m2sdr_rfic_iface.h"
#include "m2sdr_si5351_i2c.h"
#include "platform.h"

/* Defines */
/*---------*/

#define AD9361_GPIO_RESET_PIN 0
#define M2SDR_TX_GAIN_MIN_DB -89
#define M2SDR_TX_GAIN_MAX_DB   0
#define M2SDR_RX_GAIN_MIN_DB   0
#define M2SDR_RX_GAIN_MAX_DB  76
#define M2SDR_AD9361_FIR_PROFILE_MAX 16

#ifndef M2SDR_LOG_ENABLED
#define M2SDR_LOG_ENABLED 1
#endif

#if M2SDR_LOG_ENABLED
#define M2SDR_LOGF(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define M2SDR_LOGF(...) do {} while (0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define M2SDR_WEAK __attribute__((weak))
#else
#define M2SDR_WEAK
#endif

/* State */
/*-------*/

struct m2sdr_rfic_ad9361_ctx {
    char fir_profile[M2SDR_AD9361_FIR_PROFILE_MAX];
    char rx_gain_mode[2][16];
    unsigned oversampling;
};

/* The Analog Devices AD9361 code expects Linux-like platform hooks with a
 * single implicit active device. libm2sdr keeps that glue local here. */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
static _Thread_local struct m2sdr_dev *tls_rf_dev;
#elif defined(__GNUC__) || defined(__clang__)
static __thread struct m2sdr_dev *tls_rf_dev;
#else
static struct m2sdr_dev *tls_rf_dev;
#endif

/* Helpers */
/*---------*/

/* Return the raw backend connection object expected by the low-level AD9361
 * SPI and SI5351 helpers. */
static void *m2sdr_conn(struct m2sdr_dev *dev)
{
#ifdef USE_LITEPCIE
    return (void *)(intptr_t)dev->fd;
#elif defined(USE_LITEETH)
    return dev->eb;
#else
    (void)dev;
    return NULL;
#endif
}

static int m2sdr_from_ad9361_rc(int32_t rc)
{
    return (rc == 0) ? M2SDR_ERR_OK : M2SDR_ERR_IO;
}

static int m2sdr_select_rf_dev(struct m2sdr_dev *dev)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    tls_rf_dev = dev;
    return M2SDR_ERR_OK;
}

static struct ad9361_rf_phy *m2sdr_current_phy(struct m2sdr_dev *dev)
{
    return dev ? dev->ad9361_phy : NULL;
}

static int m2sdr_rfic_ad9361_set_sample_rate(struct m2sdr_dev *dev, void *ctx, int64_t rate);

/* Experimental 1x FIR candidates for the 122.88 MSPS oversampling mode.
 * These widen the FIR passband versus the legacy BladeRF-derived RX filter.
 * They are symmetric 64-tap LPFs and should be treated as A/B test profiles. */
static const int16_t m2sdr_fir_1x_wide_taps[64] = {
     -65,     15,    109,    -15,   -166,      9,    240,      4,
    -331,    -30,    442,     72,   -577,   -135,    739,    228,
    -934,   -359,   1173,    544,  -1473,   -811,   1867,   1208,
   -2425,  -1847,   3323,   3033,  -5141,  -6052,  11568,  32767,
   32767,  11568,  -6052,  -5141,   3033,   3323,  -1847,  -2425,
    1208,   1867,   -811,  -1473,    544,   1173,   -359,   -934,
     228,    739,   -135,   -577,     72,    442,    -30,   -331,
       4,    240,      9,   -166,    -15,    109,     15,    -65,
};

static void m2sdr_fir_set_64_taps(int16_t *dst, const int16_t *src)
{
    memset(dst, 0, 128 * sizeof(int16_t));
    memcpy(dst, src, 64 * sizeof(int16_t));
}

static AD9361_RXFIRConfig m2sdr_make_rx_fir_from_64(const int16_t *taps)
{
    AD9361_RXFIRConfig cfg = rx_fir_config;

    cfg.rx      = 3;
    cfg.rx_gain = -6;
    cfg.rx_dec  = 1;
    m2sdr_fir_set_64_taps(cfg.rx_coef, taps);
    cfg.rx_coef_size = 64;
    return cfg;
}

static AD9361_TXFIRConfig m2sdr_make_tx_fir_from_64(const int16_t *taps, int tx_gain_db)
{
    AD9361_TXFIRConfig cfg = tx_fir_config;

    cfg.tx      = 3;
    cfg.tx_gain = tx_gain_db;
    cfg.tx_int  = 1;
    m2sdr_fir_set_64_taps(cfg.tx_coef, taps);
    cfg.tx_coef_size = 64;
    return cfg;
}

static AD9361_RXFIRConfig m2sdr_make_rx_fir_bypass(void)
{
    AD9361_RXFIRConfig cfg = rx_fir_config;

    cfg.rx      = 3;
    cfg.rx_gain = -6;
    cfg.rx_dec  = 1;
    memset(cfg.rx_coef, 0, sizeof(cfg.rx_coef));
    cfg.rx_coef[0] = 32767;
    cfg.rx_coef_size = 64;
    return cfg;
}

static AD9361_TXFIRConfig m2sdr_make_tx_fir_bypass(void)
{
    AD9361_TXFIRConfig cfg = tx_fir_config;

    cfg.tx      = 3;
    cfg.tx_gain = 0;
    cfg.tx_int  = 1;
    memset(cfg.tx_coef, 0, sizeof(cfg.tx_coef));
    cfg.tx_coef[0] = 32767;
    cfg.tx_coef_size = 64;
    return cfg;
}

static int m2sdr_ad9361_canonical_gain_mode(const char *value, char *out, size_t out_len)
{
    if (!value || !out || out_len == 0)
        return M2SDR_ERR_INVAL;

    if (strcmp(value, "slow") == 0 || strcmp(value, "slowattack") == 0)
        return (snprintf(out, out_len, "%s", "slowattack") >= (int)out_len) ? M2SDR_ERR_RANGE : M2SDR_ERR_OK;
    else if (strcmp(value, "fast") == 0 || strcmp(value, "fastattack") == 0)
        return (snprintf(out, out_len, "%s", "fastattack") >= (int)out_len) ? M2SDR_ERR_RANGE : M2SDR_ERR_OK;
    else if (strcmp(value, "hybrid") == 0)
        return (snprintf(out, out_len, "%s", "hybrid") >= (int)out_len) ? M2SDR_ERR_RANGE : M2SDR_ERR_OK;
    else if (strcmp(value, "manual") == 0 || strcmp(value, "mgc") == 0)
        return (snprintf(out, out_len, "%s", "manual") >= (int)out_len) ? M2SDR_ERR_RANGE : M2SDR_ERR_OK;
    else
        return M2SDR_ERR_PARSE;
}

static uint8_t m2sdr_ad9361_gain_mode_to_api(const char *mode)
{
    if (strcmp(mode, "slowattack") == 0)
        return RF_GAIN_SLOWATTACK_AGC;
    if (strcmp(mode, "fastattack") == 0)
        return RF_GAIN_FASTATTACK_AGC;
    if (strcmp(mode, "hybrid") == 0)
        return RF_GAIN_HYBRID_AGC;
    return RF_GAIN_MGC;
}

static int m2sdr_ad9361_select_fir_profile(const char *name,
                                           AD9361_RXFIRConfig *rx_cfg,
                                           AD9361_TXFIRConfig *tx_cfg,
                                           char *canonical_name,
                                           size_t canonical_name_len)
{
    if (!name || !rx_cfg || !tx_cfg || !canonical_name || canonical_name_len == 0)
        return M2SDR_ERR_INVAL;

    if (name[0] == '\0' || strcmp(name, "legacy") == 0) {
        *rx_cfg = rx_fir_config;
        *tx_cfg = tx_fir_config;
        snprintf(canonical_name, canonical_name_len, "legacy");
        return M2SDR_ERR_OK;
    }
    if (strcmp(name, "bypass") == 0) {
        *rx_cfg = m2sdr_make_rx_fir_bypass();
        *tx_cfg = m2sdr_make_tx_fir_bypass();
        snprintf(canonical_name, canonical_name_len, "bypass");
        return M2SDR_ERR_OK;
    }
    if (strcmp(name, "match") == 0) {
        *rx_cfg = rx_fir_config;
        *tx_cfg = m2sdr_make_tx_fir_from_64(rx_fir_config.rx_coef, -6);
        snprintf(canonical_name, canonical_name_len, "match");
        return M2SDR_ERR_OK;
    }
    if (strcmp(name, "wide") == 0) {
        *rx_cfg = m2sdr_make_rx_fir_from_64(m2sdr_fir_1x_wide_taps);
        *tx_cfg = m2sdr_make_tx_fir_from_64(m2sdr_fir_1x_wide_taps, -6);
        snprintf(canonical_name, canonical_name_len, "wide");
        return M2SDR_ERR_OK;
    }

    return M2SDR_ERR_PARSE;
}

static int m2sdr_require_phy(struct m2sdr_dev *dev, struct ad9361_rf_phy **phy_out)
{
    struct ad9361_rf_phy *phy;

    if (!dev || !phy_out)
        return M2SDR_ERR_INVAL;
    if (m2sdr_select_rf_dev(dev) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;

    phy = m2sdr_current_phy(dev);
    if (!phy)
        return M2SDR_ERR_STATE;

    *phy_out = phy;
    return M2SDR_ERR_OK;
}

static int m2sdr_validate_config_values(const struct m2sdr_config *cfg)
{
    if (!cfg)
        return M2SDR_ERR_INVAL;

    if (cfg->sample_rate <= 0 || cfg->sample_rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    if (cfg->bandwidth <= 0 || cfg->bandwidth > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    if (cfg->refclk_freq <= 0 || cfg->refclk_freq > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    if (cfg->tx_freq <= 0 || cfg->rx_freq <= 0)
        return M2SDR_ERR_RANGE;
    if (cfg->tx_gain < M2SDR_TX_GAIN_MIN_DB || cfg->tx_gain > M2SDR_TX_GAIN_MAX_DB)
        return M2SDR_ERR_RANGE;
    if (cfg->rx_gain1 < M2SDR_RX_GAIN_MIN_DB || cfg->rx_gain1 > M2SDR_RX_GAIN_MAX_DB)
        return M2SDR_ERR_RANGE;
    if (cfg->rx_gain2 < M2SDR_RX_GAIN_MIN_DB || cfg->rx_gain2 > M2SDR_RX_GAIN_MAX_DB)
        return M2SDR_ERR_RANGE;

    return M2SDR_ERR_OK;
}

/* Resolve the effective clock source, honoring legacy string overrides when
 * older callers still populate sync_mode. */
static int m2sdr_clock_source_from_config(const struct m2sdr_config *cfg,
                                          enum m2sdr_clock_source *source)
{
    if (!cfg || !source)
        return M2SDR_ERR_INVAL;

    /* Typed enums are preferred, but the old utilities still pass string
     * overrides through chan_mode/sync_mode. Keep that compatibility here so
     * the rest of the RF path can stay strictly typed. */
    if (cfg->sync_mode) {
        if (strcmp(cfg->sync_mode, "internal") == 0) {
            *source = M2SDR_CLOCK_SOURCE_INTERNAL;
            return M2SDR_ERR_OK;
        }
        if (strcmp(cfg->sync_mode, "external") == 0) {
            *source = M2SDR_CLOCK_SOURCE_EXTERNAL;
            return M2SDR_ERR_OK;
        }
        return M2SDR_ERR_INVAL;
    }

    *source = cfg->clock_source;
    return M2SDR_ERR_OK;
}

/* Resolve the requested RF channel layout, again accepting the legacy
 * chan_mode string used by older utilities. */
static int m2sdr_channel_layout_from_config(const struct m2sdr_config *cfg,
                                            enum m2sdr_channel_layout *layout)
{
    if (!cfg || !layout)
        return M2SDR_ERR_INVAL;

    if (cfg->chan_mode) {
        if (strcmp(cfg->chan_mode, "1t1r") == 0) {
            *layout = M2SDR_CHANNEL_LAYOUT_1T1R;
            return M2SDR_ERR_OK;
        }
        if (strcmp(cfg->chan_mode, "2t2r") == 0) {
            *layout = M2SDR_CHANNEL_LAYOUT_2T2R;
            return M2SDR_ERR_OK;
        }
        return M2SDR_ERR_INVAL;
    }

    *layout = cfg->channel_layout;
    return M2SDR_ERR_OK;
}

/* Apply the selected 1T1R/2T2R topology both to the AD9361 init parameters and
 * to the FPGA-side PHY control register. */
static int m2sdr_apply_channel_layout(struct m2sdr_dev *dev,
                                       enum m2sdr_channel_layout channel_layout)
{
    if (channel_layout == M2SDR_CHANNEL_LAYOUT_1T1R) {
        /* The AD9361 init structure and the FPGA-side PHY control CSR must be
         * kept in sync or the datapath framing becomes inconsistent. */
        M2SDR_LOGF("Setting Channel Mode to 1T1R.\n");
        default_init_param.two_rx_two_tx_mode_enable     = 0;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
        default_init_param.two_t_two_r_timing_enable     = 0;
        if (m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
    }

    if (channel_layout == M2SDR_CHANNEL_LAYOUT_2T2R) {
        M2SDR_LOGF("Setting Channel Mode to 2T2R.\n");
        default_init_param.two_rx_two_tx_mode_enable     = 1;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 1;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 1;
        default_init_param.two_t_two_r_timing_enable     = 1;
        if (m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, 0) != 0)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

/* Program the SI5351 clock generator for the selected reference topology. */
static int m2sdr_configure_clocking(struct m2sdr_dev *dev,
                                     void *conn,
                                     const struct m2sdr_config *cfg,
                                     enum m2sdr_clock_source clock_source)
{
#ifdef CSR_SI5351_BASE
    M2SDR_LOGF("Initializing SI5351 Clocking...\n");

    if (clock_source == M2SDR_CLOCK_SOURCE_INTERNAL) {
        /* The table selection is driven only by the requested AD9361 refclk.
         * The SI5351 profile itself encodes the rest of the clock tree. */
        M2SDR_LOGF("Using internal XO as SI5351 CLKIN source...\n");
        if (m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
            SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET)) != 0)
            return M2SDR_ERR_IO;

        if (cfg->refclk_freq == 40000000) {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_xo_40m_config,
                sizeof(si5351_xo_40m_config) / sizeof(si5351_xo_40m_config[0]));
        } else {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_xo_38p4m_config,
                sizeof(si5351_xo_38p4m_config) / sizeof(si5351_xo_38p4m_config[0]));
        }
    } else if (clock_source == M2SDR_CLOCK_SOURCE_EXTERNAL) {
        M2SDR_LOGF("Using external 10MHz as SI5351 CLKIN source...\n");
        if (m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
              SI5351C_VERSION               * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET) |
              SI5351C_10MHZ_CLK_IN_FROM_UFL * (1 << CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET)) != 0)
            return M2SDR_ERR_IO;

        if (cfg->refclk_freq == 40000000) {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_clkin_10m_40m_config,
                sizeof(si5351_clkin_10m_40m_config) / sizeof(si5351_clkin_10m_40m_config[0]));
        } else {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_clkin_10m_38p4m_config,
                sizeof(si5351_clkin_10m_38p4m_config) / sizeof(si5351_clkin_10m_38p4m_config[0]));
        }
    }
#else
    (void)dev;
    (void)conn;
    (void)cfg;
    (void)clock_source;
#endif
    return M2SDR_ERR_OK;
}

/* Apply a common RF bandwidth to both RX and TX paths. */
static int m2sdr_configure_bandwidth(struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    M2SDR_LOGF("Setting TX/RX Bandwidth to %f MHz.\n", cfg->bandwidth / 1e6);
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_bandwidth(phy, cfg->bandwidth)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_rf_bandwidth(phy, cfg->bandwidth)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Program the TX and RX local oscillator frequencies. */
static int m2sdr_configure_frequencies(struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    M2SDR_LOGF("Setting TX LO Freq to %f MHz.\n", cfg->tx_freq / 1e6);
    M2SDR_LOGF("Setting RX LO Freq to %f MHz.\n", cfg->rx_freq / 1e6);
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_lo_freq(phy, cfg->tx_freq)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_lo_freq(phy, cfg->rx_freq)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Apply the cached RX gain-control modes before pushing explicit RX gains. */
static int m2sdr_configure_gain_modes(struct m2sdr_rfic_ad9361_ctx *ad9361,
                                      struct ad9361_rf_phy *phy)
{
    unsigned channel;

    if (!ad9361 || !phy)
        return M2SDR_ERR_INVAL;

    for (channel = 0; channel < 2; channel++) {
        const char *mode = ad9361->rx_gain_mode[channel];

        if (!mode[0])
            mode = "manual";
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(
                phy, channel, m2sdr_ad9361_gain_mode_to_api(mode))) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

/* Apply TX attenuation and per-channel RX gains. */
static int m2sdr_configure_gains(struct m2sdr_rfic_ad9361_ctx *ad9361,
                                 struct ad9361_rf_phy *phy,
                                 const struct m2sdr_config *cfg)
{
    int tx_rc;
    int rx0_rc;
    int rx1_rc;
    int rc;

    rc = m2sdr_configure_gain_modes(ad9361, phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    M2SDR_LOGF("Setting TX Gain to %ld dB.\n", (long)cfg->tx_gain);
    tx_rc = ad9361_set_tx_atten(phy, (uint32_t)(-cfg->tx_gain * 1000), 1, 1, 1);
    if (m2sdr_from_ad9361_rc(tx_rc) != M2SDR_ERR_OK)
        M2SDR_LOGF("Warning: AD9361 TX gain configuration failed (rc=%d), continuing.\n",
                   tx_rc);

    M2SDR_LOGF("Setting RX Gain to %ld dB and %ld dB.\n",
           (long)cfg->rx_gain1, (long)cfg->rx_gain2);
    rx0_rc = ad9361_set_rx_rf_gain(phy, 0, cfg->rx_gain1);
    if (m2sdr_from_ad9361_rc(rx0_rc) != M2SDR_ERR_OK)
        M2SDR_LOGF("Warning: AD9361 RX0 gain configuration failed (rc=%d), continuing.\n",
                   rx0_rc);
    rx1_rc = ad9361_set_rx_rf_gain(phy, 1, cfg->rx_gain2);
    if (m2sdr_from_ad9361_rc(rx1_rc) != M2SDR_ERR_OK)
        M2SDR_LOGF("Warning: AD9361 RX1 gain configuration failed (rc=%d), continuing.\n",
                   rx1_rc);
    return M2SDR_ERR_OK;
}

/* Configure the FPGA-side bit mode and AD9361 internal loopback controls. */
static int m2sdr_configure_modes(struct m2sdr_dev *dev, struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    int loopback_rc;

    M2SDR_LOGF("Setting Loopback to %d\n", cfg->loopback);
    /* Keep loopback configuration best-effort for compatibility with the
     * original utility, which never treated ad9361_bist_loopback() failures
     * as fatal during bring-up. Some boards return an error even on the
     * default "disable loopback" path while the rest of the RF setup works. */
    loopback_rc = ad9361_bist_loopback(phy, cfg->loopback);
    if (m2sdr_from_ad9361_rc(loopback_rc) != M2SDR_ERR_OK)
        M2SDR_LOGF("Warning: AD9361 loopback configuration failed (rc=%d), continuing.\n",
                   loopback_rc);

    /* Bit mode is implemented in the FPGA-side AD9361 wrapper rather than in
     * the AD9361 itself, so it is applied through a CSR write here. */
    if (cfg->enable_8bit_mode)
        M2SDR_LOGF("Enabling 8-bit mode.\n");
    else
        M2SDR_LOGF("Enabling 16-bit mode.\n");

    if (m2sdr_reg_write(dev, CSR_AD9361_BITMODE_ADDR, cfg->enable_8bit_mode ? 1 : 0) != 0)
        return M2SDR_ERR_IO;
    dev->iq_bits = cfg->enable_8bit_mode ? 8u : 12u;

    return M2SDR_ERR_OK;
}

/* Run the optional AD9361 built-in test modes requested by the config. */
static int m2sdr_run_bist(const struct m2sdr_config *cfg, struct m2sdr_dev *dev, struct ad9361_rf_phy *phy)
{
    if (cfg->bist_tx_tone) {
        M2SDR_LOGF("BIST_TX_TONE_TEST...\n");
        if (m2sdr_from_ad9361_rc(ad9361_bist_tone(phy, BIST_INJ_TX, cfg->bist_tone_freq, 0, 0x0)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    if (cfg->bist_rx_tone) {
        M2SDR_LOGF("BIST_RX_TONE_TEST...\n");
        if (m2sdr_from_ad9361_rc(ad9361_bist_tone(phy, BIST_INJ_RX, cfg->bist_tone_freq, 0, 0x0)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    if (cfg->bist_prbs) {
        M2SDR_LOGF("BIST_PRBS TEST...\n");
        if (m2sdr_reg_write(dev, CSR_AD9361_PRBS_TX_ADDR,
            0 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET)) != 0)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_bist_prbs(phy, BIST_INJ_RX)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

/* AD9361 platform hooks */
/*-----------------------*/

/* Bridge the AD9361 platform SPI callback onto libm2sdr's backend-neutral
 * SPI helpers. */
int M2SDR_WEAK spi_write_then_read(struct spi_device *spi,
                                   const unsigned char *txbuf, unsigned n_tx,
                                   unsigned char *rxbuf, unsigned n_rx)
{
    void *conn;

    (void)spi;
    if (!tls_rf_dev)
        return -1;

    conn = m2sdr_conn(tls_rf_dev);

    /* The imported AD9361 driver only uses the 2-byte-address/1-byte-read and
     * 2-byte-address+1-byte-write transactions on this platform. */
    if (n_tx == 2 && n_rx == 1) {
        rxbuf[0] = m2sdr_ad9361_spi_read(conn, (txbuf[0] << 8) | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        m2sdr_ad9361_spi_write(conn, (txbuf[0] << 8) | txbuf[1], txbuf[2]);
    } else {
        return -1;
    }

    return 0;
}

/* AD9361 platform delay hook in microseconds. */
void M2SDR_WEAK udelay(unsigned long usecs)
{
    usleep(usecs);
}

/* AD9361 platform delay hook in milliseconds. */
void M2SDR_WEAK mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

/* AD9361 platform sleep hook used by the imported driver code. */
unsigned long M2SDR_WEAK msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

/* Report which GPIO number is valid for the AD9361 reset hook. */
bool M2SDR_WEAK gpio_is_valid(int number)
{
    return number == AD9361_GPIO_RESET_PIN;
}

/* Placeholder GPIO setter required by the AD9361 platform layer. */
void M2SDR_WEAK gpio_set_value(unsigned gpio, int value)
{
    (void)gpio;
    (void)value;
}

static int m2sdr_rfic_ad9361_bind(struct m2sdr_dev *dev, void *ctx, void *phy)
{
    (void)ctx;

    if (!dev && phy)
        return M2SDR_ERR_INVAL;
    if (!dev)
        return M2SDR_ERR_OK;

    if (dev->ad9361_phy && phy && dev->ad9361_phy != (struct ad9361_rf_phy *)phy)
        return M2SDR_ERR_STATE;

    /* bind() is the escape hatch for advanced integrations that already own an
     * AD9361 instance. Once bound, the backend reuses that state instead of
     * calling ad9361_init() itself. */
    dev->ad9361_phy = (struct ad9361_rf_phy *)phy;
    if (phy)
        tls_rf_dev = dev;

    return M2SDR_ERR_OK;
}

/* Execute the full RF bring-up sequence: clocks, AD9361 init, rates, gains,
 * loopback, bit mode, and optional built-in tests. */
static int m2sdr_rfic_ad9361_apply_config(struct m2sdr_dev *dev, void *ctx, const struct m2sdr_config *cfg)
{
    void *conn;
    struct ad9361_rf_phy *phy;
    struct m2sdr_rfic_ad9361_ctx *ad9361 = ctx;
    int rc;
    enum m2sdr_clock_source clock_source     = M2SDR_CLOCK_SOURCE_INTERNAL;
    enum m2sdr_channel_layout channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;

    (void)ctx;

    if (!dev || !cfg || !ad9361)
        return M2SDR_ERR_INVAL;
    /* AD9361 init allocates opaque driver state with no public teardown API.
     * Refuse implicit re-init on the same device to avoid leaking that state. */
    if (dev->ad9361_phy)
        return M2SDR_ERR_STATE;
    rc = m2sdr_validate_config_values(cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;

    rc = m2sdr_select_rf_dev(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    conn     = m2sdr_conn(dev);

    /* Normalize all compatibility inputs up front so the actual bring-up code
     * only deals with one typed representation. */
    if (m2sdr_clock_source_from_config(cfg, &clock_source) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;
    if (m2sdr_channel_layout_from_config(cfg, &channel_layout) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;

    M2SDR_LOGF("Initializing AD9361 RFIC...\n");
    default_init_param.reference_clk_rate = cfg->refclk_freq;
    default_init_param.gpio_resetb        = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync          = -1;
    default_init_param.gpio_cal_sw1       = -1;
    default_init_param.gpio_cal_sw2       = -1;

    if (!cfg->bypass_rfic_init) {
        rc = m2sdr_configure_clocking(dev, conn, cfg, clock_source);
        if (rc != M2SDR_ERR_OK)
            return rc;

        M2SDR_LOGF("Initializing AD9361 SPI...\n");
        m2sdr_ad9361_spi_init(conn, 1);

        rc = m2sdr_apply_channel_layout(dev, channel_layout);
        if (rc != M2SDR_ERR_OK)
            return rc;
    }
    /* The imported AD9361 stack multiplexes two modes through ad9361_init():
     * normal hardware init, or attach to an already-running instance when
     * bypass_rfic_init is requested. Keep both flows behind apply_config() so
     * Soapy and utilities do not need their own AD9361 bootstrap logic. */
    if (m2sdr_from_ad9361_rc(ad9361_init(&dev->ad9361_phy,
                                         &default_init_param,
                                         cfg->bypass_rfic_init ? 0 : 1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    phy = m2sdr_current_phy(dev);
    if (!phy)
        return M2SDR_ERR_STATE;

    if (cfg->bypass_rfic_init) {
        dev->iq_bits = cfg->enable_8bit_mode ? 8u : 12u;
        return M2SDR_ERR_OK;
    }

    ad9361->oversampling = cfg->enable_oversample ? 1u : 0u;

    rc = m2sdr_configure_bandwidth(phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_configure_frequencies(phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;

    /* Re-apply the default FIR tables after the sample-rate step so the
     * AD9361 state stays aligned with the shipped utility behavior. */
    rc = m2sdr_rfic_ad9361_set_sample_rate(dev, ctx, cfg->sample_rate);
    if (rc != M2SDR_ERR_OK)
        return rc;

    rc = m2sdr_configure_gains(ad9361, phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_configure_modes(dev, phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_run_bist(cfg, dev, phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    return M2SDR_ERR_OK;
}

/* Program one LO frequency on the already-initialized AD9361 instance. */
static int m2sdr_rfic_ad9361_set_frequency(struct m2sdr_dev *dev, void *ctx,
                                           enum m2sdr_direction direction, uint64_t freq)
{
    struct ad9361_rf_phy *phy;
    int rc;
    (void)ctx;

    if (!dev || freq == 0)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (direction == M2SDR_TX) {
        if (m2sdr_from_ad9361_rc(ad9361_set_tx_lo_freq(phy, freq)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    } else {
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_lo_freq(phy, freq)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_get_frequency(struct m2sdr_dev *dev, void *ctx,
                                           enum m2sdr_direction direction, uint64_t *freq)
{
    struct ad9361_rf_phy *phy;
    int rc;

    (void)ctx;

    if (!dev || !freq)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (direction == M2SDR_TX) {
        if (m2sdr_from_ad9361_rc(ad9361_get_tx_lo_freq(phy, freq)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    } else {
        if (m2sdr_from_ad9361_rc(ad9361_get_rx_lo_freq(phy, freq)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

/* Program a common sample rate on both RX and TX paths. */
static int m2sdr_rfic_ad9361_set_sample_rate(struct m2sdr_dev *dev, void *ctx, int64_t rate)
{
    struct ad9361_rf_phy *phy;
    const struct m2sdr_rfic_ad9361_ctx *ad9361 = ctx;
    AD9361_RXFIRConfig rx_fir_cfg;
    AD9361_TXFIRConfig tx_fir_cfg;
    char canonical_name[M2SDR_AD9361_FIR_PROFILE_MAX];
    uint32_t actual_rate;
    int rc;

    if (!dev || !ad9361)
        return M2SDR_ERR_INVAL;
    if (rate <= 0 || rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    actual_rate = (uint32_t)rate;
    if (ad9361->oversampling) {
        if ((actual_rate % 2u) != 0u)
            return M2SDR_ERR_RANGE;
        actual_rate /= 2u;
    }

    /* Preserve the original utility behavior:
     * - below 1.25 MSPS: use x4 FIR interpolation/decimation
     * - below 2.5 MSPS: use x2 FIR interpolation/decimation
     * - otherwise: restore the selected 1x FIR profile */
    if (actual_rate < 1250000u) {
        rx_fir_cfg = rx_fir_config_dec4;
        tx_fir_cfg = tx_fir_config_int4;
        phy->rx_fir_dec    = 4;
        phy->tx_fir_int    = 4;
        phy->bypass_rx_fir = 0;
        phy->bypass_tx_fir = 0;
        rx_fir_cfg.rx_dec  = 4;
        tx_fir_cfg.tx_int  = 4;
    } else if (actual_rate < 2500000u) {
        rx_fir_cfg = rx_fir_config_dec2;
        tx_fir_cfg = tx_fir_config_int2;
        phy->rx_fir_dec    = 2;
        phy->tx_fir_int    = 2;
        phy->bypass_rx_fir = 0;
        phy->bypass_tx_fir = 0;
        rx_fir_cfg.rx_dec  = 2;
        tx_fir_cfg.tx_int  = 2;
    } else {
        rc = m2sdr_ad9361_select_fir_profile(ad9361->fir_profile,
                                             &rx_fir_cfg,
                                             &tx_fir_cfg,
                                             canonical_name,
                                             sizeof(canonical_name));
        if (rc != M2SDR_ERR_OK)
            return rc;
        phy->rx_fir_dec    = 1;
        phy->tx_fir_int    = 1;
        phy->bypass_rx_fir = 0;
        phy->bypass_tx_fir = 0;
    }

    if (m2sdr_from_ad9361_rc(ad9361_set_rx_fir_config(phy, rx_fir_cfg)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_fir_config(phy, tx_fir_cfg)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_fir_en_dis(phy, 1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_fir_en_dis(phy, 1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    if (m2sdr_from_ad9361_rc(ad9361_set_tx_sampling_freq(phy, actual_rate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_sampling_freq(phy, actual_rate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (ad9361->oversampling)
        ad9361_enable_oversampling(phy);
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_get_sample_rate(struct m2sdr_dev *dev, void *ctx, int64_t *rate)
{
    struct ad9361_rf_phy *phy;
    uint32_t value = 0;
    int rc;

    (void)ctx;

    if (!dev || !rate)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_get_rx_sampling_freq(phy, &value)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    *rate = (int64_t)value;
    return M2SDR_ERR_OK;
}

/* Program a common RF bandwidth on both RX and TX paths. */
static int m2sdr_rfic_ad9361_set_bandwidth(struct m2sdr_dev *dev, void *ctx, int64_t bw)
{
    struct ad9361_rf_phy *phy;
    int rc;
    (void)ctx;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (bw <= 0 || bw > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_bandwidth(phy, bw)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_rf_bandwidth(phy, bw)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_get_bandwidth(struct m2sdr_dev *dev, void *ctx, int64_t *bw)
{
    struct ad9361_rf_phy *phy;
    uint32_t value = 0;
    int rc;

    (void)ctx;

    if (!dev || !bw)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_get_rx_rf_bandwidth(phy, &value)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    *bw = (int64_t)value;
    return M2SDR_ERR_OK;
}

/* Apply one direction-specific gain setting to the initialized AD9361. */
static int m2sdr_rfic_ad9361_set_gain(struct m2sdr_dev *dev, void *ctx,
                                      enum m2sdr_direction direction, int64_t gain)
{
    struct ad9361_rf_phy *phy;
    int rc;
    (void)ctx;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (direction == M2SDR_TX) {
        if (gain < M2SDR_TX_GAIN_MIN_DB || gain > M2SDR_TX_GAIN_MAX_DB)
            return M2SDR_ERR_RANGE;
        if (m2sdr_from_ad9361_rc(ad9361_set_tx_atten(phy, (uint32_t)(-gain * 1000), 1, 1, 1)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    } else {
        if (gain < M2SDR_RX_GAIN_MIN_DB || gain > M2SDR_RX_GAIN_MAX_DB)
            return M2SDR_ERR_RANGE;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 0, gain)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 1, gain)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_get_gain(struct m2sdr_dev *dev, void *ctx,
                                      enum m2sdr_direction direction, int64_t *gain)
{
    struct ad9361_rf_phy *phy;
    int rc;

    (void)ctx;

    if (!dev || !gain)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (direction == M2SDR_TX) {
        uint32_t atten_mdb = 0;
        if (m2sdr_from_ad9361_rc(ad9361_get_tx_attenuation(phy, 0, &atten_mdb)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        *gain = -((int64_t)atten_mdb / 1000);
    } else {
        int32_t rx_gain = 0;
        if (m2sdr_from_ad9361_rc(ad9361_get_rx_rf_gain(phy, 0, &rx_gain)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        *gain = (int64_t)rx_gain;
    }

    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_set_iq_bits(struct m2sdr_dev *dev, void *ctx, unsigned bits)
{
    (void)ctx;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (bits != 8u && bits != 12u)
        return M2SDR_ERR_RANGE;

    if (m2sdr_reg_write(dev, CSR_AD9361_BITMODE_ADDR, bits == 8u ? 1u : 0u) != 0)
        return M2SDR_ERR_IO;

    dev->iq_bits = bits;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_get_iq_bits(struct m2sdr_dev *dev, void *ctx, unsigned *bits)
{
    (void)ctx;

    if (!dev || !bits)
        return M2SDR_ERR_INVAL;

    *bits = dev->iq_bits ? dev->iq_bits : 12u;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_configure_stream_channels(struct m2sdr_dev *dev, void *ctx,
                                                       size_t rx_count, const size_t *rx_channels,
                                                       size_t tx_count, const size_t *tx_channels)
{
    struct ad9361_rf_phy *phy;
    struct ad9361_phy_platform_data *pd;
    size_t active_count;
    size_t rx_local[2];
    size_t tx_local[2];
    enum m2sdr_channel_layout layout;
    int rc;

    (void)ctx;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if ((rx_count > 2) || (tx_count > 2))
        return M2SDR_ERR_RANGE;
    if (rx_count == 0 && tx_count == 0)
        return M2SDR_ERR_INVAL;
    active_count = rx_count ? rx_count : tx_count;
    if (active_count != 1 && active_count != 2)
        return M2SDR_ERR_RANGE;
    if ((rx_count != 0 && !rx_channels) || (tx_count != 0 && !tx_channels))
        return M2SDR_ERR_INVAL;

    if (rx_count == 0) {
        rx_count = tx_count;
        rx_local[0] = tx_channels[0];
        if (tx_count > 1)
            rx_local[1] = tx_channels[1];
        rx_channels = rx_local;
    }
    if (tx_count == 0) {
        tx_count = rx_count;
        tx_local[0] = rx_channels[0];
        if (rx_count > 1)
            tx_local[1] = rx_channels[1];
        tx_channels = tx_local;
    }

    if (active_count == 1) {
        if (rx_channels[0] > 1 || tx_channels[0] > 1)
            return M2SDR_ERR_RANGE;
    } else {
        if (!((rx_channels[0] == 0 && rx_channels[1] == 1) ||
              (rx_channels[0] == 1 && rx_channels[1] == 0)))
            return M2SDR_ERR_INVAL;
        if (!((tx_channels[0] == 0 && tx_channels[1] == 1) ||
              (tx_channels[0] == 1 && tx_channels[1] == 0)))
            return M2SDR_ERR_INVAL;
    }

    /* Soapy expresses channel selection as explicit channel lists, but the
     * AD9361 datapath still exposes it as 1T1R/2T2R plus RX/TX lane picks. Do
     * the translation once here so upper layers stay backend-neutral. */
    layout = (active_count == 2) ? M2SDR_CHANNEL_LAYOUT_2T2R : M2SDR_CHANNEL_LAYOUT_1T1R;
    rc = m2sdr_apply_channel_layout(dev, layout);
    if (rc != M2SDR_ERR_OK)
        return rc;

    phy->pdata->rx2tx2 = (active_count == 2);
    if (active_count == 1) {
        phy->pdata->rx1tx1_mode_use_rx_num = rx_channels[0] == 0 ? RX_1 : RX_2;
        phy->pdata->rx1tx1_mode_use_tx_num = tx_channels[0] == 0 ? TX_1 : TX_2;
    } else {
        phy->pdata->rx1tx1_mode_use_rx_num = RX_1 | RX_2;
        phy->pdata->rx1tx1_mode_use_tx_num = TX_1 | TX_2;
    }

    pd = phy->pdata;
    pd->port_ctrl.pp_conf[0] &= ~(1 << 2);
    if (active_count == 2)
        pd->port_ctrl.pp_conf[0] |= (1 << 2);

    if (m2sdr_from_ad9361_rc(ad9361_set_no_ch_mode(phy, active_count)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_init(struct m2sdr_dev *dev, void **ctx_out)
{
    struct m2sdr_rfic_ad9361_ctx *ctx;

    if (!dev || !ctx_out)
        return M2SDR_ERR_INVAL;
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return M2SDR_ERR_NO_MEM;
    snprintf(ctx->fir_profile, sizeof(ctx->fir_profile), "legacy");
    snprintf(ctx->rx_gain_mode[0], sizeof(ctx->rx_gain_mode[0]), "manual");
    snprintf(ctx->rx_gain_mode[1], sizeof(ctx->rx_gain_mode[1]), "manual");
    ctx->oversampling = 0u;
    *ctx_out = ctx;
    dev->ad9361_phy = NULL;
    dev->iq_bits = 12u;
    return M2SDR_ERR_OK;
}

static void m2sdr_rfic_ad9361_cleanup(struct m2sdr_dev *dev, void *ctx)
{
    struct m2sdr_rfic_ad9361_ctx *ad9361 = ctx;

    (void)ctx;
    if (!dev)
        return;
    if (tls_rf_dev == dev)
        tls_rf_dev = NULL;
    dev->ad9361_phy = NULL;
    free(ad9361);
}

static int m2sdr_rfic_ad9361_get_caps(struct m2sdr_dev *dev, void *ctx, struct m2sdr_rfic_caps *caps)
{
    (void)dev;
    (void)ctx;

    if (!caps)
        return M2SDR_ERR_INVAL;

    memset(caps, 0, sizeof(*caps));
    caps->kind = M2SDR_RFIC_KIND_AD9361;
    snprintf(caps->name, sizeof(caps->name), "ad9361");
    caps->features = M2SDR_RFIC_FEATURE_BIND_EXTERNAL |
                     M2SDR_RFIC_FEATURE_BIST |
                     M2SDR_RFIC_FEATURE_OVERSAMPLE |
                     M2SDR_RFIC_FEATURE_STREAMING |
                     M2SDR_RFIC_FEATURE_RX_GAIN_MODE |
                     M2SDR_RFIC_FEATURE_TEMP_SENSOR;
    caps->rx_channels = 2u;
    caps->tx_channels = 2u;
    caps->min_rx_frequency = 70000000LL;
    caps->max_rx_frequency = 6000000000LL;
    caps->min_tx_frequency = 47000000LL;
    caps->max_tx_frequency = 6000000000LL;
    caps->min_sample_rate = 1;
    caps->max_sample_rate = UINT32_MAX;
    caps->min_bandwidth   = 1;
    caps->max_bandwidth   = UINT32_MAX;
    caps->min_tx_gain     = M2SDR_TX_GAIN_MIN_DB;
    caps->max_tx_gain     = M2SDR_TX_GAIN_MAX_DB;
    caps->min_rx_gain     = M2SDR_RX_GAIN_MIN_DB;
    caps->max_rx_gain     = M2SDR_RX_GAIN_MAX_DB;
    caps->supported_iq_bits_mask = M2SDR_IQ_BITS_MASK(8) | M2SDR_IQ_BITS_MASK(12);
    caps->native_iq_bits = 12u;
    caps->min_iq_bits = 8u;
    caps->max_iq_bits = 12u;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_ad9361_set_property(struct m2sdr_dev *dev, void *ctx,
                                          const char *key, const char *value)
{
    struct m2sdr_rfic_ad9361_ctx *ad9361 = ctx;
    struct ad9361_rf_phy *phy = NULL;

    if (!dev || !ad9361 || !key || !value)
        return M2SDR_ERR_INVAL;

    if (strcmp(key, "ad9361.iq_bits") == 0) {
        char *end = NULL;
        unsigned long bits = strtoul(value, &end, 10);
        if (!end || *end != '\0')
            return M2SDR_ERR_PARSE;
        if (bits > UINT_MAX)
            return M2SDR_ERR_RANGE;
        return m2sdr_rfic_ad9361_set_iq_bits(dev, ctx, (unsigned)bits);
    }
    if (strcmp(key, "ad9361.oversampling") == 0) {
        /* Oversampling is cached as backend policy and consumed when the sample
         * rate is next applied. This mirrors the old Soapy behavior without
         * forcing upper layers to sequence AD9361 FIR calls directly. */
        if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0)
            ad9361->oversampling = 0u;
        else if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0)
            ad9361->oversampling = 1u;
        else
            return M2SDR_ERR_PARSE;
        return M2SDR_ERR_OK;
    }
    if (strcmp(key, "ad9361.fir_profile") == 0) {
        AD9361_RXFIRConfig rx_fir_cfg;
        AD9361_TXFIRConfig tx_fir_cfg;
        char canonical_name[M2SDR_AD9361_FIR_PROFILE_MAX];
        int rc;

        rc = m2sdr_ad9361_select_fir_profile(value,
                                             &rx_fir_cfg,
                                             &tx_fir_cfg,
                                             canonical_name,
                                             sizeof(canonical_name));
        if (rc != M2SDR_ERR_OK)
            return rc;
        /* Validate eagerly even if the PHY is not initialized yet, so callers
         * get fast feedback on bad profile names before the next rate change. */
        snprintf(ad9361->fir_profile, sizeof(ad9361->fir_profile), "%s", canonical_name);
        return M2SDR_ERR_OK;
    }
    if (strcmp(key, "ad9361.rx0_gain_mode") == 0 || strcmp(key, "ad9361.rx1_gain_mode") == 0) {
        const unsigned channel = (key[10] == '0') ? 0u : 1u;
        char canonical_mode[16];
        int rc;

        rc = m2sdr_ad9361_canonical_gain_mode(value, canonical_mode, sizeof(canonical_mode));
        if (rc != M2SDR_ERR_OK)
            return rc;
        snprintf(ad9361->rx_gain_mode[channel], sizeof(ad9361->rx_gain_mode[channel]), "%s", canonical_mode);
        /* Gain mode is both cached policy and live state: before init we keep
         * the requested mode in the backend context, after init we also push it
         * into the vendor driver immediately. */
        rc = m2sdr_require_phy(dev, &phy);
        if (rc == M2SDR_ERR_OK) {
            if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy,
                                                                     channel,
                                                                     m2sdr_ad9361_gain_mode_to_api(canonical_mode))) != M2SDR_ERR_OK)
                return M2SDR_ERR_IO;
            return M2SDR_ERR_OK;
        }
        if (rc == M2SDR_ERR_STATE)
            return M2SDR_ERR_OK;
        return rc;
    }

    return M2SDR_ERR_UNSUPPORTED;
}

static int m2sdr_rfic_ad9361_get_property(struct m2sdr_dev *dev, void *ctx,
                                          const char *key, char *value, size_t value_len)
{
    struct m2sdr_rfic_ad9361_ctx *ad9361 = ctx;

    if (!dev || !ad9361 || !key || !value || value_len == 0)
        return M2SDR_ERR_INVAL;

    if (strcmp(key, "ad9361.iq_bits") == 0) {
        unsigned bits = 0;
        int rc = m2sdr_rfic_ad9361_get_iq_bits(dev, ctx, &bits);
        if (rc != M2SDR_ERR_OK)
            return rc;
        if (snprintf(value, value_len, "%u", bits) >= (int)value_len)
            return M2SDR_ERR_RANGE;
        return M2SDR_ERR_OK;
    }
    if (strcmp(key, "ad9361.oversampling") == 0) {
        if (snprintf(value, value_len, "%u", ad9361->oversampling ? 1u : 0u) >= (int)value_len)
            return M2SDR_ERR_RANGE;
        return M2SDR_ERR_OK;
    }
    if (strcmp(key, "ad9361.fir_profile") == 0) {
        if (snprintf(value, value_len, "%s", ad9361->fir_profile) >= (int)value_len)
            return M2SDR_ERR_RANGE;
        return M2SDR_ERR_OK;
    }
    if (strcmp(key, "ad9361.rx0_gain_mode") == 0 || strcmp(key, "ad9361.rx1_gain_mode") == 0) {
        if (snprintf(value, value_len, "%s",
                     ad9361->rx_gain_mode[(key[10] == '0') ? 0u : 1u]) >= (int)value_len)
            return M2SDR_ERR_RANGE;
        return M2SDR_ERR_OK;
    }
    if (strcmp(key, "ad9361.temperature_c") == 0) {
        struct ad9361_rf_phy *phy = NULL;
        int rc = m2sdr_require_phy(dev, &phy);
        if (rc != M2SDR_ERR_OK)
            return rc;
        if (snprintf(value, value_len, "%.3f", (double)ad9361_get_temp(phy) / 1000.0) >= (int)value_len)
            return M2SDR_ERR_RANGE;
        return M2SDR_ERR_OK;
    }

    return M2SDR_ERR_UNSUPPORTED;
}

const struct m2sdr_rfic_ops m2sdr_rfic_ad9361_ops = {
    .kind           = M2SDR_RFIC_KIND_AD9361,
    .name           = "ad9361",
    .init           = m2sdr_rfic_ad9361_init,
    .cleanup        = m2sdr_rfic_ad9361_cleanup,
    .bind           = m2sdr_rfic_ad9361_bind,
    .apply_config   = m2sdr_rfic_ad9361_apply_config,
    .set_frequency  = m2sdr_rfic_ad9361_set_frequency,
    .get_frequency  = m2sdr_rfic_ad9361_get_frequency,
    .set_sample_rate= m2sdr_rfic_ad9361_set_sample_rate,
    .get_sample_rate= m2sdr_rfic_ad9361_get_sample_rate,
    .set_bandwidth  = m2sdr_rfic_ad9361_set_bandwidth,
    .get_bandwidth  = m2sdr_rfic_ad9361_get_bandwidth,
    .set_gain       = m2sdr_rfic_ad9361_set_gain,
    .get_gain       = m2sdr_rfic_ad9361_get_gain,
    .set_iq_bits    = m2sdr_rfic_ad9361_set_iq_bits,
    .get_iq_bits    = m2sdr_rfic_ad9361_get_iq_bits,
    .configure_stream_channels = m2sdr_rfic_ad9361_configure_stream_channels,
    .get_caps       = m2sdr_rfic_ad9361_get_caps,
    .set_property   = m2sdr_rfic_ad9361_set_property,
    .get_property   = m2sdr_rfic_ad9361_get_property,
};
