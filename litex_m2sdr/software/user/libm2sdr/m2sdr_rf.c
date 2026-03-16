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
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "ad9361.h"
#include "ad9361_api.h"
#include "csr.h"
#include "m2sdr_ad9361_spi.h"
#include "m2sdr_config.h"
#include "m2sdr_internal.h"
#include "m2sdr_si5351_i2c.h"
#include "platform.h"

/* Defines */
/*---------*/

#define AD9361_GPIO_RESET_PIN 0
#define M2SDR_TX_ATT_MIN_DB    0
#define M2SDR_TX_ATT_MAX_DB   89
#define M2SDR_RX_GAIN_MIN_DB   0
#define M2SDR_RX_GAIN_MAX_DB  76

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
    if (cfg->tx_att < M2SDR_TX_ATT_MIN_DB || cfg->tx_att > M2SDR_TX_ATT_MAX_DB)
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

/* Configure the AD9361 sampling clocks and, for very low rates, the x4 FIR
 * interpolation/decimation mode expected by the original utilities. */
static int m2sdr_configure_samplerate(struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    uint32_t actual_samplerate = cfg->sample_rate;

    M2SDR_LOGF("Setting TX/RX Samplerate to %f MSPS.\n", cfg->sample_rate / 1e6);

    if (cfg->enable_oversample)
        actual_samplerate /= 2;

    /* Match the historical utility behavior: for the lowest rates, switch the
     * AD9361 FIRs to x4 interpolation/decimation before programming clocks. */
    if (actual_samplerate < 2500000) {
        AD9361_RXFIRConfig rx_fir_cfg = rx_fir_config;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config;

        M2SDR_LOGF("Setting TX/RX FIR Interpolation/Decimation to 4 (< 2.5 Msps Samplerate).\n");
        phy->rx_fir_dec    = 4;
        phy->tx_fir_int    = 4;
        phy->bypass_rx_fir = 0;
        phy->bypass_tx_fir = 0;
        rx_fir_cfg.rx_dec         = 4;
        tx_fir_cfg.tx_int         = 4;

        if (m2sdr_from_ad9361_rc(ad9361_set_rx_fir_config(phy, rx_fir_cfg)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_tx_fir_config(phy, tx_fir_cfg)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_fir_en_dis(phy, 1)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_tx_fir_en_dis(phy, 1)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    if (m2sdr_from_ad9361_rc(ad9361_set_tx_sampling_freq(phy, actual_samplerate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_sampling_freq(phy, actual_samplerate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

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

/* Apply TX attenuation and per-channel RX gains. */
static int m2sdr_configure_gains(struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    /* The AD9361 only accepts explicit RX gain writes in manual gain-control
     * mode. Ensure both channels are in MGC before applying the requested
     * gains so the standalone m2sdr_rf flow remains compatible. */
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 0, RF_GAIN_MGC)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 1, RF_GAIN_MGC)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    M2SDR_LOGF("Setting TX Attenuation to %ld dB.\n", (long)cfg->tx_att);
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_atten(phy, (uint32_t)(cfg->tx_att * 1000), 1, 1, 1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    M2SDR_LOGF("Setting RX Gain to %ld dB and %ld dB.\n",
           (long)cfg->rx_gain1, (long)cfg->rx_gain2);
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 0, cfg->rx_gain1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 1, cfg->rx_gain2)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Configure the FPGA-side bit mode and AD9361 internal loopback controls. */
static int m2sdr_configure_modes(struct m2sdr_dev *dev, struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    M2SDR_LOGF("Setting Loopback to %d\n", cfg->loopback);
    if (m2sdr_from_ad9361_rc(ad9361_bist_loopback(phy, cfg->loopback)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    /* Bit mode is implemented in the FPGA-side AD9361 wrapper rather than in
     * the AD9361 itself, so it is applied through a CSR write here. */
    if (cfg->enable_8bit_mode)
        M2SDR_LOGF("Enabling 8-bit mode.\n");
    else
        M2SDR_LOGF("Enabling 16-bit mode.\n");

    if (m2sdr_reg_write(dev, CSR_AD9361_BITMODE_ADDR, cfg->enable_8bit_mode ? 1 : 0) != 0)
        return M2SDR_ERR_IO;

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

/* Public API */
/*------------*/

/* Fill a config structure with the default RF values used by the standalone
 * utilities and examples. */
void m2sdr_config_init(struct m2sdr_config *cfg)
{
    if (!cfg)
        return;

    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate       = DEFAULT_SAMPLERATE;
    cfg->bandwidth         = DEFAULT_BANDWIDTH;
    cfg->refclk_freq       = DEFAULT_REFCLK_FREQ;
    cfg->tx_freq           = DEFAULT_TX_FREQ;
    cfg->rx_freq           = DEFAULT_RX_FREQ;
    cfg->tx_att            = DEFAULT_TX_ATT;
    cfg->rx_gain1          = DEFAULT_RX_GAIN;
    cfg->rx_gain2          = DEFAULT_RX_GAIN;
    cfg->loopback          = DEFAULT_LOOPBACK;
    cfg->bist_tone_freq    = DEFAULT_BIST_TONE_FREQ;
    cfg->bist_tx_tone      = false;
    cfg->bist_rx_tone      = false;
    cfg->bist_prbs         = false;
    cfg->enable_8bit_mode  = false;
    cfg->enable_oversample = false;
    cfg->channel_layout    = M2SDR_CHANNEL_LAYOUT_2T2R;
    cfg->clock_source      = M2SDR_CLOCK_SOURCE_INTERNAL;
    cfg->chan_mode         = NULL;
    cfg->sync_mode         = NULL;
}

/* Bind an externally-initialized AD9361 instance to libm2sdr, primarily for
 * the Soapy integration path. */
int m2sdr_rf_bind(struct m2sdr_dev *dev, void *phy)
{
    if (!dev && phy)
        return M2SDR_ERR_INVAL;
    if (!dev)
        return M2SDR_ERR_OK;

    if (dev->ad9361_phy && phy && dev->ad9361_phy != (struct ad9361_rf_phy *)phy)
        return M2SDR_ERR_STATE;

    dev->ad9361_phy = (struct ad9361_rf_phy *)phy;
    if (phy)
        tls_rf_dev = dev;

    return M2SDR_ERR_OK;
}

/* Execute the full RF bring-up sequence: clocks, AD9361 init, rates, gains,
 * loopback, bit mode, and optional built-in tests. */
int m2sdr_apply_config(struct m2sdr_dev *dev, const struct m2sdr_config *cfg)
{
    void *conn;
    struct ad9361_rf_phy *phy;
    int rc;
    enum m2sdr_clock_source clock_source     = M2SDR_CLOCK_SOURCE_INTERNAL;
    enum m2sdr_channel_layout channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;

    if (!dev || !cfg)
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

    rc = m2sdr_configure_clocking(dev, conn, cfg, clock_source);
    if (rc != M2SDR_ERR_OK)
        return rc;

    M2SDR_LOGF("Initializing AD9361 SPI...\n");
    m2sdr_ad9361_spi_init(conn, 1);

    M2SDR_LOGF("Initializing AD9361 RFIC...\n");
    default_init_param.reference_clk_rate = cfg->refclk_freq;
    default_init_param.gpio_resetb        = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync          = -1;
    default_init_param.gpio_cal_sw1       = -1;
    default_init_param.gpio_cal_sw2       = -1;

    rc = m2sdr_apply_channel_layout(dev, channel_layout);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (m2sdr_from_ad9361_rc(ad9361_init(&dev->ad9361_phy, &default_init_param, 1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    phy = m2sdr_current_phy(dev);
    if (!phy)
        return M2SDR_ERR_STATE;

    rc = m2sdr_configure_samplerate(phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_configure_bandwidth(phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_configure_frequencies(phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;

    /* Re-apply the default FIR tables after the sample-rate step so the
     * AD9361 state stays aligned with the shipped utility behavior. */
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_fir_config(phy, tx_fir_config)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_fir_config(phy, rx_fir_config)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    rc = m2sdr_configure_gains(phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_configure_modes(dev, phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_run_bist(cfg, dev, phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (cfg->enable_oversample)
        ad9361_enable_oversampling(phy);

    return M2SDR_ERR_OK;
}

/* Program one LO frequency on the already-initialized AD9361 instance. */
int m2sdr_set_frequency(struct m2sdr_dev *dev, enum m2sdr_direction direction, uint64_t freq)
{
    struct ad9361_rf_phy *phy;
    int rc;

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

/* Program a common sample rate on both RX and TX paths. */
int m2sdr_set_sample_rate(struct m2sdr_dev *dev, int64_t rate)
{
    struct ad9361_rf_phy *phy;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (rate <= 0 || rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_set_tx_sampling_freq(phy, (uint32_t)rate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_sampling_freq(phy, (uint32_t)rate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Program a common RF bandwidth on both RX and TX paths. */
int m2sdr_set_bandwidth(struct m2sdr_dev *dev, int64_t bw)
{
    struct ad9361_rf_phy *phy;
    int rc;

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

/* Apply one direction-specific gain setting to the initialized AD9361. */
int m2sdr_set_gain(struct m2sdr_dev *dev, enum m2sdr_direction direction, int64_t gain)
{
    struct ad9361_rf_phy *phy;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (direction == M2SDR_TX) {
        if (gain < M2SDR_TX_ATT_MIN_DB || gain > M2SDR_TX_ATT_MAX_DB)
            return M2SDR_ERR_RANGE;
        if (m2sdr_from_ad9361_rc(ad9361_set_tx_atten(phy, (uint32_t)(gain * 1000), 1, 1, 1)) != M2SDR_ERR_OK)
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

/* Convenience wrapper for the RX LO frequency setter. */
int m2sdr_set_rx_frequency(struct m2sdr_dev *dev, uint64_t freq)
{
    return m2sdr_set_frequency(dev, M2SDR_RX, freq);
}

/* Convenience wrapper for the TX LO frequency setter. */
int m2sdr_set_tx_frequency(struct m2sdr_dev *dev, uint64_t freq)
{
    return m2sdr_set_frequency(dev, M2SDR_TX, freq);
}

/* Convenience wrapper for the RX gain setter. */
int m2sdr_set_rx_gain(struct m2sdr_dev *dev, int64_t gain)
{
    return m2sdr_set_gain(dev, M2SDR_RX, gain);
}

int m2sdr_set_rx_gain_chan(struct m2sdr_dev *dev, unsigned channel, int64_t gain)
{
    struct ad9361_rf_phy *phy;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (channel > 1)
        return M2SDR_ERR_RANGE;
    if (gain < M2SDR_RX_GAIN_MIN_DB || gain > M2SDR_RX_GAIN_MAX_DB)
        return M2SDR_ERR_RANGE;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, channel, gain)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
}

int m2sdr_set_tx_att(struct m2sdr_dev *dev, int64_t attenuation_db)
{
    return m2sdr_set_gain(dev, M2SDR_TX, attenuation_db);
}
