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
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

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
#define M2SDR_DELAY_TAPS      16
#define M2SDR_DELAY_SETTLE_MS 10
#define SI5351_STATUS_SYS_INIT 0x80
#define SI5351_STATUS_LOL_B    0x40
#define SI5351_STATUS_LOL_A    0x20
#define SI5351_STATUS_LOS      0x10
#define SI5351_READY_TIMEOUT_MS 100
#ifdef USE_LITEETH
#define M2SDR_LITEETH_RF_INIT_TIMEOUT_MS 5000
#endif

#ifndef M2SDR_LOG_ENABLED
#define M2SDR_LOG_ENABLED 1
#endif

static int m2sdr_log_enabled = M2SDR_LOG_ENABLED ? 1 : 0;
#define M2SDR_LOGF(...) m2sdr_log_printf(__VA_ARGS__)

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
#ifdef USE_LITEETH
static _Thread_local uint64_t tls_rf_init_deadline_us;
static _Thread_local bool tls_rf_init_timed_out;
#endif
#elif defined(__GNUC__) || defined(__clang__)
static __thread struct m2sdr_dev *tls_rf_dev;
#ifdef USE_LITEETH
static __thread uint64_t tls_rf_init_deadline_us;
static __thread bool tls_rf_init_timed_out;
#endif
#else
static struct m2sdr_dev *tls_rf_dev;
#ifdef USE_LITEETH
static uint64_t tls_rf_init_deadline_us;
static bool tls_rf_init_timed_out;
#endif
#endif

/* Helpers */
/*---------*/

int m2sdr_set_log_enabled(bool enable)
{
    m2sdr_log_enabled = enable ? 1 : 0;
    return M2SDR_ERR_OK;
}

int m2sdr_log_is_enabled(void)
{
    return m2sdr_log_enabled;
}

void m2sdr_log_printf(const char *fmt, ...)
{
    va_list ap;

    if (!m2sdr_log_enabled || !fmt)
        return;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Return the raw backend connection object expected by the low-level AD9361
 * SPI and SI5351 helpers. */
static void *m2sdr_conn(struct m2sdr_dev *dev)
{
    return m2sdr_get_handle(dev);
}

#ifdef USE_LITEETH
static uint64_t m2sdr_time_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void m2sdr_start_rf_init_timeout(void)
{
    uint64_t now = m2sdr_time_us();

    tls_rf_init_timed_out = false;
    tls_rf_init_deadline_us = now ?
        now + (uint64_t)M2SDR_LITEETH_RF_INIT_TIMEOUT_MS * 1000u : 0;
}

static void m2sdr_clear_rf_init_timeout(void)
{
    tls_rf_init_deadline_us = 0;
}

static bool m2sdr_rf_init_timeout_expired(void)
{
    uint64_t now;

    if (!tls_rf_init_deadline_us)
        return false;

    now = m2sdr_time_us();
    return now && now >= tls_rf_init_deadline_us;
}
#endif

static int m2sdr_from_ad9361_rc(int32_t rc)
{
    return (rc == 0) ? M2SDR_ERR_OK : M2SDR_ERR_IO;
}

static int m2sdr_to_ad9361_gain_mode(enum m2sdr_rx_gain_mode mode, uint8_t *ad9361_mode)
{
    if (!ad9361_mode)
        return M2SDR_ERR_INVAL;

    switch (mode) {
    case M2SDR_RX_GAIN_MODE_MANUAL:
        *ad9361_mode = RF_GAIN_MGC;
        return M2SDR_ERR_OK;
    case M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC:
        *ad9361_mode = RF_GAIN_SLOWATTACK_AGC;
        return M2SDR_ERR_OK;
    case M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC:
        *ad9361_mode = RF_GAIN_FASTATTACK_AGC;
        return M2SDR_ERR_OK;
    case M2SDR_RX_GAIN_MODE_HYBRID_AGC:
        *ad9361_mode = RF_GAIN_HYBRID_AGC;
        return M2SDR_ERR_OK;
    default:
        return M2SDR_ERR_INVAL;
    }
}

static int m2sdr_from_ad9361_gain_mode(uint8_t ad9361_mode, enum m2sdr_rx_gain_mode *mode)
{
    if (!mode)
        return M2SDR_ERR_INVAL;

    switch (ad9361_mode) {
    case RF_GAIN_MGC:
        *mode = M2SDR_RX_GAIN_MODE_MANUAL;
        return M2SDR_ERR_OK;
    case RF_GAIN_SLOWATTACK_AGC:
        *mode = M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC;
        return M2SDR_ERR_OK;
    case RF_GAIN_FASTATTACK_AGC:
        *mode = M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC;
        return M2SDR_ERR_OK;
    case RF_GAIN_HYBRID_AGC:
        *mode = M2SDR_RX_GAIN_MODE_HYBRID_AGC;
        return M2SDR_ERR_OK;
    default:
        return M2SDR_ERR_UNSUPPORTED;
    }
}

const char *m2sdr_rx_gain_mode_name(enum m2sdr_rx_gain_mode mode)
{
    switch (mode) {
    case M2SDR_RX_GAIN_MODE_MANUAL:
        return "manual";
    case M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC:
        return "slow";
    case M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC:
        return "fast";
    case M2SDR_RX_GAIN_MODE_HYBRID_AGC:
        return "hybrid";
    default:
        return "unknown";
    }
}

int m2sdr_parse_rx_gain_mode(const char *text, enum m2sdr_rx_gain_mode *mode)
{
    if (!text || !mode)
        return M2SDR_ERR_INVAL;

    if (strcasecmp(text, "manual") == 0 || strcasecmp(text, "mgc") == 0) {
        *mode = M2SDR_RX_GAIN_MODE_MANUAL;
        return M2SDR_ERR_OK;
    }
    if (strcasecmp(text, "slow") == 0 || strcasecmp(text, "slowattack") == 0 ||
        strcasecmp(text, "slow-attack") == 0 || strcasecmp(text, "slow-agc") == 0) {
        *mode = M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC;
        return M2SDR_ERR_OK;
    }
    if (strcasecmp(text, "fast") == 0 || strcasecmp(text, "fastattack") == 0 ||
        strcasecmp(text, "fast-attack") == 0 || strcasecmp(text, "fast-agc") == 0) {
        *mode = M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC;
        return M2SDR_ERR_OK;
    }
    if (strcasecmp(text, "hybrid") == 0 || strcasecmp(text, "hybrid-agc") == 0) {
        *mode = M2SDR_RX_GAIN_MODE_HYBRID_AGC;
        return M2SDR_ERR_OK;
    }

    return M2SDR_ERR_PARSE;
}

static int m2sdr_transport_error(struct m2sdr_dev *dev)
{
#ifdef USE_LITEETH
    if (dev && dev->eb) {
        int err = eb_get_last_error(dev->eb);

        if (err == EB_ERR_TIMEOUT || err == EB_ERR_INTERRUPTED)
            return M2SDR_ERR_TIMEOUT;
    }
#endif
    (void)dev;
    return M2SDR_ERR_IO;
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
    uint8_t ad9361_mode;

    if (!cfg)
        return M2SDR_ERR_INVAL;

    if (cfg->sample_rate <= 0 || cfg->sample_rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    if (cfg->bandwidth <= 0 || cfg->bandwidth > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    if (cfg->refclk_freq <= 0 || cfg->refclk_freq > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    /* Written to also reject NaN. */
    if (!(cfg->refclk_ppm >= -M2SDR_REFCLK_PPM_MAX &&
          cfg->refclk_ppm <=  M2SDR_REFCLK_PPM_MAX))
        return M2SDR_ERR_RANGE;
    if (cfg->tx_freq <= 0 || cfg->rx_freq <= 0)
        return M2SDR_ERR_RANGE;
    if (cfg->tx_att < M2SDR_TX_ATT_MIN_DB || cfg->tx_att > M2SDR_TX_ATT_MAX_DB)
        return M2SDR_ERR_RANGE;
    if (cfg->rx_gain1 < M2SDR_RX_GAIN_MIN_DB || cfg->rx_gain1 > M2SDR_RX_GAIN_MAX_DB)
        return M2SDR_ERR_RANGE;
    if (cfg->rx_gain2 < M2SDR_RX_GAIN_MIN_DB || cfg->rx_gain2 > M2SDR_RX_GAIN_MAX_DB)
        return M2SDR_ERR_RANGE;
    if (cfg->program_rx_gain_modes &&
        (m2sdr_to_ad9361_gain_mode(cfg->rx_gain_mode1, &ad9361_mode) != M2SDR_ERR_OK ||
         m2sdr_to_ad9361_gain_mode(cfg->rx_gain_mode2, &ad9361_mode) != M2SDR_ERR_OK))
        return M2SDR_ERR_INVAL;

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
        if ((strcmp(cfg->sync_mode, "fpga") == 0) ||
            (strcmp(cfg->sync_mode, "si5351c-fpga") == 0) ||
            (strcmp(cfg->sync_mode, "si5351c_fpga") == 0) ||
            (strcmp(cfg->sync_mode, "pll") == 0)) {
            *source = M2SDR_CLOCK_SOURCE_SI5351C_FPGA;
            return M2SDR_ERR_OK;
        }
        return M2SDR_ERR_INVAL;
    }

    switch (cfg->clock_source) {
    case M2SDR_CLOCK_SOURCE_INTERNAL:
    case M2SDR_CLOCK_SOURCE_EXTERNAL:
    case M2SDR_CLOCK_SOURCE_SI5351C_FPGA:
        *source = cfg->clock_source;
        return M2SDR_ERR_OK;
    default:
        return M2SDR_ERR_INVAL;
    }
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

/* Lazily allocate the per-device AD9361 init-parameter snapshot, seeded from
 * the built-in defaults. default_init_param is a per-translation-unit
 * template (static in m2sdr_config.h): mutating it is invisible to other
 * files and races between devices, so all configuration goes through this
 * per-device copy instead. */
static AD9361_InitParam *m2sdr_dev_init_param(struct m2sdr_dev *dev)
{
    if (!dev->rf_init_param) {
        dev->rf_init_param = malloc(sizeof(AD9361_InitParam));
        if (dev->rf_init_param)
            memcpy(dev->rf_init_param, &default_init_param,
                   sizeof(AD9361_InitParam));
    }
    return (AD9361_InitParam *)dev->rf_init_param;
}

int m2sdr_rf_store_init_param(struct m2sdr_dev *dev,
                              const void *init_param, size_t size)
{
    if (!dev || !init_param)
        return M2SDR_ERR_INVAL;
    if (size != sizeof(AD9361_InitParam))
        return M2SDR_ERR_INVAL;
    if (!dev->rf_init_param) {
        dev->rf_init_param = malloc(size);
        if (!dev->rf_init_param)
            return M2SDR_ERR_NO_MEM;
    }
    memcpy(dev->rf_init_param, init_param, size);
    return M2SDR_ERR_OK;
}

/* Apply the selected 1T1R/2T2R topology both to the AD9361 init parameters and
 * to the FPGA-side PHY control register. rx/tx_channel select the physical
 * channel used in 1T1R (0 = RX1/TX1, 1 = RX2/TX2). */
static int m2sdr_apply_channel_layout(struct m2sdr_dev *dev,
                                       AD9361_InitParam *init_param,
                                       enum m2sdr_channel_layout channel_layout,
                                       unsigned rx_channel, unsigned tx_channel)
{
    if (channel_layout == M2SDR_CHANNEL_LAYOUT_1T1R) {
        /* The AD9361 init structure and the FPGA-side PHY control CSR must be
         * kept in sync or the datapath framing becomes inconsistent. */
        M2SDR_LOGF("Setting Channel Mode to 1T1R.\n");
        init_param->two_rx_two_tx_mode_enable     = 0;
        init_param->one_rx_one_tx_mode_use_rx_num = rx_channel == 0 ? 1 : 2;
        init_param->one_rx_one_tx_mode_use_tx_num = tx_channel == 0 ? 1 : 2;
        init_param->two_t_two_r_timing_enable     = 0;
        if (m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
    }

    if (channel_layout == M2SDR_CHANNEL_LAYOUT_2T2R) {
        M2SDR_LOGF("Setting Channel Mode to 2T2R.\n");
        init_param->two_rx_two_tx_mode_enable     = 1;
        init_param->one_rx_one_tx_mode_use_rx_num = 1;
        init_param->one_rx_one_tx_mode_use_tx_num = 1;
        init_param->two_t_two_r_timing_enable     = 1;
        if (m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, 0) != 0)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

/* Convert the stream rate the host observes to the rate programmed into the
 * AD9361. The only divisor is the FPGA oversample mode (x2). Note: 1T1R does
 * NOT change this relationship - the stream rate equals the AD9361 rate in
 * both layouts. (An earlier dual-channel-enable bug made 1T1R appear to run
 * at twice the programmed rate; the port then carried both RX channels.) */
static int m2sdr_stream_to_ad9361_sample_rate(int64_t stream_rate,
                                              bool half_rate,
                                              uint32_t *ad9361_rate)
{
    uint64_t divisor = 1;
    uint64_t rounded;

    if (!ad9361_rate || stream_rate <= 0 || stream_rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;

    if (half_rate)
        divisor *= 2;

    rounded = ((uint64_t)stream_rate + divisor / 2) / divisor;
    if (rounded == 0 || rounded > UINT32_MAX)
        return M2SDR_ERR_RANGE;

    *ad9361_rate = (uint32_t)rounded;
    return M2SDR_ERR_OK;
}

#ifdef CSR_SI5351_BASE
/* Select the precomputed SI5351 register table for a reference topology. */
static void m2sdr_si5351_select_config(enum m2sdr_clock_source clock_source,
                                       int64_t refclk_freq,
                                       const uint8_t (**config)[2],
                                       size_t *length)
{
    if (clock_source == M2SDR_CLOCK_SOURCE_INTERNAL) {
        if (refclk_freq == 40000000) {
            *config = si5351_xo_40m_config;
            *length = sizeof(si5351_xo_40m_config) / sizeof(si5351_xo_40m_config[0]);
        } else {
            *config = si5351_xo_38p4m_config;
            *length = sizeof(si5351_xo_38p4m_config) / sizeof(si5351_xo_38p4m_config[0]);
        }
    } else {
        if (refclk_freq == 40000000) {
            *config = si5351_clkin_10m_40m_config;
            *length = sizeof(si5351_clkin_10m_40m_config) / sizeof(si5351_clkin_10m_40m_config[0]);
        } else {
            *config = si5351_clkin_10m_38p4m_config;
            *length = sizeof(si5351_clkin_10m_38p4m_config) / sizeof(si5351_clkin_10m_38p4m_config[0]);
        }
    }
}

static int m2sdr_wait_si5351_ready(struct m2sdr_dev *dev, void *conn)
{
    uint8_t status = 0xff;

    if (!m2sdr_si5351_i2c_check_litei2c(conn))
        return M2SDR_ERR_OK;

    /* All shipped M2SDR SI5351 tables use PLLB for the active outputs. PLLA
     * and CLKIN LOS can legitimately remain asserted with the internal-XO
     * profile, so gate the AD9361 handoff on SYS_INIT and PLLB only. */
    for (int timeout = SI5351_READY_TIMEOUT_MS; timeout > 0; timeout--) {
        if (!m2sdr_si5351_i2c_read(conn, SI5351_I2C_ADDR, 0x00, &status, 1, true))
            return m2sdr_transport_error(dev);

        if ((status & (SI5351_STATUS_SYS_INIT | SI5351_STATUS_LOL_B)) == 0) {
            if (status & (SI5351_STATUS_LOL_A | SI5351_STATUS_LOS))
                M2SDR_LOGF("SI5351 status after clocking: 0x%02x (continuing; PLLB is locked).\n", status);
            mdelay(M2SDR_DELAY_SETTLE_MS);
            return M2SDR_ERR_OK;
        }

        usleep(1000);
    }

    M2SDR_LOGF("SI5351 did not report ready before AD9361 init (status 0x%02x).\n", status);
    return M2SDR_ERR_IO;
}
#endif

/* Program the SI5351 clock generator for the selected reference topology. */
static int m2sdr_configure_clocking(struct m2sdr_dev *dev,
                                     void *conn,
                                     const struct m2sdr_config *cfg,
                                     enum m2sdr_clock_source clock_source)
{
#ifdef CSR_SI5351_BASE
    const uint8_t (*si5351_config)[2];
    size_t si5351_length;

    M2SDR_LOGF("Initializing SI5351 Clocking...\n");

    if (clock_source == M2SDR_CLOCK_SOURCE_INTERNAL) {
        M2SDR_LOGF("Using internal XO as SI5351 CLKIN source...\n");
        if (m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
            SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET)) != 0)
            return M2SDR_ERR_IO;
    } else if (clock_source == M2SDR_CLOCK_SOURCE_EXTERNAL) {
        M2SDR_LOGF("Using external 10MHz from uFL as SI5351C CLKIN source...\n");
        if (m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
              SI5351C_VERSION               * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET) |
              SI5351C_10MHZ_CLK_IN_FROM_UFL * (1 << CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET)) != 0)
            return M2SDR_ERR_IO;
    } else if (clock_source == M2SDR_CLOCK_SOURCE_SI5351C_FPGA) {
        M2SDR_LOGF("Using FPGA 10MHz as SI5351C CLKIN source...\n");
        if (m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
              SI5351C_VERSION                * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET) |
              SI5351C_10MHZ_CLK_IN_FROM_PLL * (1 << CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET)) != 0)
            return M2SDR_ERR_IO;
    }

    /* The table selection is driven only by the clock source and requested
     * AD9361 refclk. The SI5351 profile itself encodes the rest of the clock
     * tree. */
    m2sdr_si5351_select_config(clock_source, cfg->refclk_freq,
                               &si5351_config, &si5351_length);
    if (!m2sdr_si5351_i2c_config_checked(conn, SI5351_I2C_ADDR,
        si5351_config, si5351_length))
        return m2sdr_transport_error(dev);

    /* Compensate the measured reference error by trimming the PLL feedback
     * away from the nominal table, correcting the AD9361 reference and all
     * derived clocks together. */
    if (cfg->refclk_ppm != 0) {
        M2SDR_LOGF("Trimming SI5351 PLL by %.3f ppm.\n", cfg->refclk_ppm);
        if (!m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
            si5351_config, si5351_length, cfg->refclk_ppm))
            return m2sdr_transport_error(dev);
    }

    return m2sdr_wait_si5351_ready(dev, conn);
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
static int m2sdr_configure_samplerate(struct ad9361_rf_phy *phy,
                                      const struct m2sdr_config *cfg,
                                      enum m2sdr_channel_layout channel_layout)
{
    uint32_t actual_samplerate;
    int rc;

    M2SDR_LOGF("Setting TX/RX Samplerate to %f MSPS.\n", cfg->sample_rate / 1e6);

    (void)channel_layout;
    /* Rates above the AD9361 clock-chain limit (61.44 MSPS, up to 122.88) run
     * the chip's clock chain at half rate with the data port at 2x
     * (wide-bandwidth mode, see ad9361_enable_oversampling()). */
    rc = m2sdr_stream_to_ad9361_sample_rate(cfg->sample_rate,
        cfg->sample_rate > 61440000, &actual_samplerate);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (actual_samplerate != (uint32_t)cfg->sample_rate) {
        M2SDR_LOGF("Programming AD9361 Samplerate to %f MSPS (wide-bandwidth mode).\n",
            actual_samplerate / 1e6);
    }

    if (actual_samplerate > 61440000U) {
        fprintf(stderr, "Requested sample rate %.3f MSPS exceeds the maximum of 122.880 MSPS.\n",
            cfg->sample_rate / 1e6);
        return M2SDR_ERR_INVAL;
    }

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
static int m2sdr_configure_gains(struct ad9361_rf_phy *phy,
                                 const struct m2sdr_config *cfg,
                                 enum m2sdr_channel_layout channel_layout)
{
    /* In 1T1R only the first channel is active: configuring the gain-control
     * mode of the inactive channel re-enables it (ad9361_set_rx_gain_control_mode
     * ends in ad9361_en_dis_rx), which corrupts the 1R port framing and the
     * RX stream delivers only zeros. */
    const bool second_channel = (channel_layout == M2SDR_CHANNEL_LAYOUT_2T2R);

    M2SDR_LOGF("Setting TX Attenuation to %ld dB.\n", (long)cfg->tx_att);
    if (m2sdr_from_ad9361_rc(ad9361_set_tx_atten(phy, (uint32_t)(cfg->tx_att * 1000), 1, 1, 1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    if (!cfg->program_rx_gains && !cfg->program_rx_gain_modes) {
        M2SDR_LOGF("Leaving RX gain mode and gains at AD9361 defaults.\n");
        return M2SDR_ERR_OK;
    }

    if (!cfg->program_rx_gains) {
        uint8_t mode1;
        uint8_t mode2;

        if (m2sdr_to_ad9361_gain_mode(cfg->rx_gain_mode1, &mode1) != M2SDR_ERR_OK ||
            m2sdr_to_ad9361_gain_mode(cfg->rx_gain_mode2, &mode2) != M2SDR_ERR_OK)
            return M2SDR_ERR_INVAL;

        M2SDR_LOGF("Setting RX gain control mode to %s and %s.\n",
            m2sdr_rx_gain_mode_name(cfg->rx_gain_mode1),
            m2sdr_rx_gain_mode_name(cfg->rx_gain_mode2));
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 0, mode1)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 1, mode2)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        return M2SDR_ERR_OK;
    }

    /* The AD9361 only accepts explicit RX gain writes in manual gain-control
     * mode. Only switch to MGC when the caller explicitly requested manual RX
     * gains through the standalone RF configuration interface. */
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 0, RF_GAIN_MGC)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (second_channel) {
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 1, RF_GAIN_MGC)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        M2SDR_LOGF("Setting RX Gain to %ld dB and %ld dB.\n",
               (long)cfg->rx_gain1, (long)cfg->rx_gain2);
    } else {
        M2SDR_LOGF("Setting RX Gain to %ld dB.\n", (long)cfg->rx_gain1);
    }
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 0, cfg->rx_gain1)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (second_channel) {
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 1, cfg->rx_gain2)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }
    return M2SDR_ERR_OK;
}

/* Configure the FPGA-side bit mode and AD9361 internal loopback controls. */
static int m2sdr_configure_modes(struct m2sdr_dev *dev, struct ad9361_rf_phy *phy, const struct m2sdr_config *cfg)
{
    enum m2sdr_format format = cfg->sample_format;

    if (cfg->enable_8bit_mode && format == M2SDR_FORMAT_SC16_Q11)
        format = M2SDR_FORMAT_SC8_Q7;

    M2SDR_LOGF("Setting Loopback to %d\n", cfg->loopback);
    if (m2sdr_from_ad9361_rc(ad9361_bist_loopback(phy, cfg->loopback)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    /* Bit mode is implemented in the FPGA-side AD9361 wrapper rather than in
     * the AD9361 itself, so it is applied through a CSR write here. */
    if (format == M2SDR_FORMAT_BFP8_Q11)
        M2SDR_LOGF("Enabling BFP8 mode.\n");
    else if (format == M2SDR_FORMAT_SC8_Q7)
        M2SDR_LOGF("Enabling 8-bit mode.\n");
    else
        M2SDR_LOGF("Enabling 16-bit mode.\n");

    int rc = m2sdr_set_sample_format(dev, format);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (cfg->program_agc_pin)
        return m2sdr_set_agc_pin(dev, cfg->agc_pin_enable);

    return M2SDR_ERR_OK;
}

/* Run the optional AD9361 built-in test modes requested by the config. */
static int m2sdr_write_prbs_tx_ctrl(struct m2sdr_dev *dev, uint32_t value)
{
    return (m2sdr_reg_write(dev, CSR_AD9361_PRBS_TX_ADDR, value) == 0) ? M2SDR_ERR_OK : M2SDR_ERR_IO;
}

static int m2sdr_read_prbs_synced(struct m2sdr_dev *dev, bool *synced)
{
    uint32_t value = 0;

    if (!dev || !synced)
        return M2SDR_ERR_INVAL;
    if (m2sdr_reg_read(dev, CSR_AD9361_PRBS_RX_ADDR, &value) != 0)
        return M2SDR_ERR_IO;

    *synced = ((value >> CSR_AD9361_PRBS_RX_SYNCED_OFFSET) &
        ((1u << CSR_AD9361_PRBS_RX_SYNCED_SIZE) - 1u)) != 0;
    return M2SDR_ERR_OK;
}

static int m2sdr_program_delay_reg(struct ad9361_rf_phy *phy, bool tx, unsigned clk_delay, unsigned data_delay, bool clock_changed)
{
    uint8_t reg_value;
    int rc;

    if (!phy || clk_delay >= M2SDR_DELAY_TAPS || data_delay >= M2SDR_DELAY_TAPS)
        return M2SDR_ERR_INVAL;

    reg_value = tx ? (uint8_t)(FB_CLK_DELAY(clk_delay) | TX_DATA_DELAY(data_delay))
                   : (uint8_t)(DATA_CLK_DELAY(clk_delay) | RX_DATA_DELAY(data_delay));

    if (clock_changed)
        ad9361_ensm_force_state(phy, ENSM_STATE_ALERT);

    rc = m2sdr_from_ad9361_rc(ad9361_spi_write(phy->spi,
        tx ? REG_TX_CLOCK_DATA_DELAY : REG_RX_CLOCK_DATA_DELAY,
        reg_value));

    if (clock_changed)
        ad9361_ensm_force_state(phy, ENSM_STATE_FDD);

    return rc;
}

static int m2sdr_scan_delay_matrix(struct m2sdr_dev *dev, struct ad9361_rf_phy *phy,
                                   bool tx, unsigned rx_clk_delay, unsigned rx_data_delay,
                                   unsigned valid[M2SDR_DELAY_TAPS][M2SDR_DELAY_TAPS])
{
    const char *label = tx ? "TX" : "RX";
    unsigned clk_delay;
    unsigned data_delay;

    printf("\n%s Clk/Dat delays scan...\n", label);
    printf("-------------------------\n");
    printf("Clk/Dat |  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15\n");

    for (clk_delay = 0; clk_delay < M2SDR_DELAY_TAPS; clk_delay++) {
        printf(" %2u     |", clk_delay);
        for (data_delay = 0; data_delay < M2SDR_DELAY_TAPS; data_delay++) {
            bool synced = false;
            int rc;

            if (tx) {
                /* TX tuning uses the FPGA PRBS generator/checker across the
                 * AD9361 loopback path, so keep the previously selected RX
                 * delay fixed while sweeping the TX interface. */
                rc = m2sdr_program_delay_reg(phy, false, rx_clk_delay, rx_data_delay, data_delay == 0);
                if (rc != M2SDR_ERR_OK)
                    return rc;
            }

            rc = m2sdr_program_delay_reg(phy, tx, clk_delay, data_delay, data_delay == 0);
            if (rc != M2SDR_ERR_OK)
                return rc;

            /* For TX tuning we rely on the FPGA PRBS generator and FPGA checker.
             * Restart the generator for each point so the checker can reacquire
             * from a known seed/phase after the delay change. */
            if (tx) {
                rc = m2sdr_write_prbs_tx_ctrl(dev, 0);
                if (rc != M2SDR_ERR_OK)
                    return rc;
                mdelay(1);
                rc = m2sdr_write_prbs_tx_ctrl(dev, 1u << CSR_AD9361_PRBS_TX_ENABLE_OFFSET);
                if (rc != M2SDR_ERR_OK)
                    return rc;
            }

            mdelay(M2SDR_DELAY_SETTLE_MS);

            rc = m2sdr_read_prbs_synced(dev, &synced);
            if (rc != M2SDR_ERR_OK)
                return rc;

            valid[clk_delay][data_delay] = synced ? 1u : 0u;
            printf(" %2u", valid[clk_delay][data_delay]);
        }
        printf("\n");
    }

    return M2SDR_ERR_OK;
}

static bool m2sdr_pick_delay_midpoint(const unsigned valid[M2SDR_DELAY_TAPS][M2SDR_DELAY_TAPS],
                                      unsigned *best_clk, unsigned *best_data, unsigned *best_run)
{
    unsigned clk_delay;
    unsigned data_delay;
    unsigned longest = 0;
    unsigned chosen_clk = 0;
    unsigned chosen_data = 0;
    bool found = false;

    for (clk_delay = 0; clk_delay < M2SDR_DELAY_TAPS; clk_delay++) {
        data_delay = 0;
        while (data_delay < M2SDR_DELAY_TAPS) {
            unsigned start;
            unsigned run;

            while (data_delay < M2SDR_DELAY_TAPS && !valid[clk_delay][data_delay])
                data_delay++;
            if (data_delay >= M2SDR_DELAY_TAPS)
                break;

            start = data_delay;
            while (data_delay < M2SDR_DELAY_TAPS && valid[clk_delay][data_delay])
                data_delay++;
            run = data_delay - start;

            if (run > longest) {
                longest = run;
                chosen_clk = clk_delay;
                chosen_data = start + run / 2;
                found = true;
            }
        }
    }

    if (!found)
        return false;

    if (best_clk)
        *best_clk = chosen_clk;
    if (best_data)
        *best_data = chosen_data;
    if (best_run)
        *best_run = longest;
    return true;
}

static int m2sdr_calibrate_interface_delay(struct m2sdr_dev *dev, struct ad9361_rf_phy *phy)
{
    unsigned rx_valid[M2SDR_DELAY_TAPS][M2SDR_DELAY_TAPS] = {{0}};
    unsigned tx_valid[M2SDR_DELAY_TAPS][M2SDR_DELAY_TAPS] = {{0}};
    unsigned best_rx_clk = 0;
    unsigned best_rx_data = 0;
    unsigned best_tx_clk = 0;
    unsigned best_tx_data = 0;
    unsigned best_rx_run = 0;
    unsigned best_tx_run = 0;
    uint32_t saved_prbs_tx_ctrl = 0;
    int32_t saved_loopback = 0;
    enum ad9361_bist_mode saved_prbs_mode = BIST_DISABLE;
    uint8_t saved_rx_delay = 0;
    uint8_t saved_tx_delay = 0;
    int rc = M2SDR_ERR_OK;

    if (!dev || !phy)
        return M2SDR_ERR_INVAL;
    if (m2sdr_reg_read(dev, CSR_AD9361_PRBS_TX_ADDR, &saved_prbs_tx_ctrl) != 0)
        return M2SDR_ERR_IO;

    ad9361_get_bist_loopback(phy, &saved_loopback);
    ad9361_get_bist_prbs(phy, &saved_prbs_mode);
    saved_rx_delay = ad9361_spi_read(phy->spi, REG_RX_CLOCK_DATA_DELAY);
    saved_tx_delay = ad9361_spi_read(phy->spi, REG_TX_CLOCK_DATA_DELAY);

    printf("Calibrating FPGA <-> AD9361 digital interface with PRBS...\n");

    rc = m2sdr_write_prbs_tx_ctrl(dev, 0);
    if (rc != M2SDR_ERR_OK)
        goto restore;
    if (m2sdr_from_ad9361_rc(ad9361_bist_prbs(phy, BIST_INJ_RX)) != M2SDR_ERR_OK) {
        rc = M2SDR_ERR_IO;
        goto restore;
    }

    rc = m2sdr_scan_delay_matrix(dev, phy, false, 0, 0, rx_valid);
    if (rc != M2SDR_ERR_OK)
        goto restore;
    if (!m2sdr_pick_delay_midpoint(rx_valid, &best_rx_clk, &best_rx_data, &best_rx_run)) {
        fprintf(stderr, "No valid RX Clk/Dat delay settings found.\n");
        rc = M2SDR_ERR_IO;
        goto restore;
    }
    rc = m2sdr_program_delay_reg(phy, false, best_rx_clk, best_rx_data, true);
    if (rc != M2SDR_ERR_OK)
        goto restore;
    printf("Selected RX Clk Delay: %u, RX Dat Delay: %u (window: %u)\n",
        best_rx_clk, best_rx_data, best_rx_run);

    if (m2sdr_from_ad9361_rc(ad9361_bist_prbs(phy, BIST_DISABLE)) != M2SDR_ERR_OK) {
        rc = M2SDR_ERR_IO;
        goto restore;
    }
    if (m2sdr_from_ad9361_rc(ad9361_bist_loopback(phy, 1)) != M2SDR_ERR_OK) {
        rc = M2SDR_ERR_IO;
        goto restore;
    }
    rc = m2sdr_write_prbs_tx_ctrl(dev, 1u << CSR_AD9361_PRBS_TX_ENABLE_OFFSET);
    if (rc != M2SDR_ERR_OK)
        goto restore;
    mdelay(M2SDR_DELAY_SETTLE_MS);

    rc = m2sdr_scan_delay_matrix(dev, phy, true, best_rx_clk, best_rx_data, tx_valid);
    if (rc != M2SDR_ERR_OK)
        goto restore;
    if (!m2sdr_pick_delay_midpoint(tx_valid, &best_tx_clk, &best_tx_data, &best_tx_run)) {
        fprintf(stderr, "No valid TX Clk/Dat delay settings found, keeping calibrated RX delay and restoring previous TX delay.\n");
        (void)ad9361_spi_write(phy->spi, REG_TX_CLOCK_DATA_DELAY, saved_tx_delay);
        printf("\nCalibration completed:\n");
        printf("----------------------\n");
        printf("RX Clk:%u/Dat:%u, TX unchanged\n", best_rx_clk, best_rx_data);
        rc = M2SDR_ERR_OK;
        goto restore;
    }
    rc = m2sdr_program_delay_reg(phy, true, best_tx_clk, best_tx_data, true);
    if (rc != M2SDR_ERR_OK)
        goto restore;
    printf("Selected TX Clk Delay: %u, TX Dat Delay: %u (window: %u)\n",
        best_tx_clk, best_tx_data, best_tx_run);

    printf("\nCalibration completed:\n");
    printf("----------------------\n");
    printf("RX Clk:%u/Dat:%u, TX Clk:%u/Dat:%u\n",
        best_rx_clk, best_rx_data, best_tx_clk, best_tx_data);

restore:
    if (rc != M2SDR_ERR_OK) {
        (void)ad9361_spi_write(phy->spi, REG_RX_CLOCK_DATA_DELAY, saved_rx_delay);
        (void)ad9361_spi_write(phy->spi, REG_TX_CLOCK_DATA_DELAY, saved_tx_delay);
    }
    if (m2sdr_write_prbs_tx_ctrl(dev, saved_prbs_tx_ctrl) != M2SDR_ERR_OK)
        rc = M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_bist_prbs(phy, saved_prbs_mode)) != M2SDR_ERR_OK)
        rc = M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_bist_loopback(phy, saved_loopback)) != M2SDR_ERR_OK)
        rc = M2SDR_ERR_IO;

    return rc;
}

/* In-mode digital interface alignment verify for the wide-bandwidth mode.
 *
 * The doubled-DATA_CLK interface can come up misaligned (the capture lands
 * on the wrong DDR/frame phase, yielding full-scale noise-like samples); the
 * chip<->FPGA framing phase is re-rolled by each clock programming, so the
 * caller retries the rate programming until this verify passes. Two probes,
 * both read the FPGA PRBS RX checker:
 *
 *  1. AD9361 BIST PRBS injected at the RX path: validates RX capture framing
 *     end-to-end. In 1R1T mode this requires the gateware's interleaved
 *     (1R1T-aware) checker; on older gateware this probe never syncs and the
 *     verify falls through to probe 2.
 *  2. FPGA PRBS TX through the AD9361 data-port loopback: in 1R1T mode each
 *     RX lane sees consecutive PRBS values, so the stock per-lane checkers
 *     can sync; also exercises the TX direction. Acquisition is restarted a
 *     few times (the checker must catch the seed state to lock).
 *
 * A synced verdict has shown no false positives; a failed verdict can be a
 * false negative on probe-2-only gateware, costing one retry.
 *
 * Returns M2SDR_ERR_OK if aligned, M2SDR_ERR_IO if not (retry). */
static int m2sdr_wide_bandwidth_verify(struct m2sdr_dev *dev)
{
    struct ad9361_rf_phy *phy;
    uint32_t saved_prbs_tx = 0;
    uint8_t obs, bist;
    bool synced = false;
    int attempt, i;
    int rc;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (m2sdr_reg_read(dev, CSR_AD9361_PRBS_TX_ADDR, &saved_prbs_tx) != 0)
        return M2SDR_ERR_IO;
    obs  = ad9361_spi_read(phy->spi, REG_OBSERVE_CONFIG);
    bist = ad9361_spi_read(phy->spi, REG_BIST_CONFIG);

    /* Probe 1: chip BIST PRBS -> RX path -> FPGA checker. */
    (void)m2sdr_write_prbs_tx_ctrl(dev, 0);
    ad9361_spi_write(phy->spi, REG_BIST_CONFIG, BIST_CTRL_POINT(2) | BIST_ENABLE);
    for (i = 0; i < 40 && !synced; i++) {
        mdelay(5);
        (void)m2sdr_read_prbs_synced(dev, &synced);
    }
    ad9361_spi_write(phy->spi, REG_BIST_CONFIG, 0x00);
    if (synced)
        M2SDR_LOGF("Wide-bandwidth verify: RX BIST PRBS synced.\n");

    /* Probe 2: FPGA PRBS TX -> chip data-port loopback -> FPGA checker. */
    if (!synced) {
        ad9361_spi_write(phy->spi, REG_OBSERVE_CONFIG,
            (uint8_t)((obs & ~DATA_PORT_SP_HD_LOOP_TEST_OE) | DATA_PORT_LOOP_TEST_ENABLE));
        for (attempt = 0; attempt < 4 && !synced; attempt++) {
            (void)m2sdr_write_prbs_tx_ctrl(dev, 0);
            mdelay(2);
            (void)m2sdr_write_prbs_tx_ctrl(dev, 1u << CSR_AD9361_PRBS_TX_ENABLE_OFFSET);
            for (i = 0; i < 10 && !synced; i++) {
                mdelay(10);
                (void)m2sdr_read_prbs_synced(dev, &synced);
            }
        }
        ad9361_spi_write(phy->spi, REG_OBSERVE_CONFIG, obs);
        if (synced)
            M2SDR_LOGF("Wide-bandwidth verify: TX loopback PRBS synced.\n");
    }

    (void)m2sdr_write_prbs_tx_ctrl(dev, saved_prbs_tx);
    ad9361_spi_write(phy->spi, REG_BIST_CONFIG, bist);

    if (!synced)
        M2SDR_LOGF("Wide-bandwidth verify: interface NOT aligned.\n");
    return synced ? M2SDR_ERR_OK : M2SDR_ERR_IO;
}

/* Program the AD9361 sampling clocks for the wide-bandwidth mode and verify
 * the doubled-rate interface, retrying in place: the interface framing phase
 * is re-rolled by each clock programming, so a misaligned outcome is
 * recovered by simply programming again. The verify briefly drives the chip
 * BIST/loopback paths, so a concurrent stream glitches while this runs. */
static int m2sdr_wide_bandwidth_bringup(struct m2sdr_dev *dev,
                                        struct ad9361_rf_phy *phy,
                                        uint32_t ad9361_rate)
{
    int rc = M2SDR_ERR_IO;
    int try_;

    for (try_ = 1; try_ <= 20; try_++) {
        if (m2sdr_from_ad9361_rc(ad9361_set_tx_sampling_freq(phy, ad9361_rate)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_sampling_freq(phy, ad9361_rate)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        ad9361_enable_oversampling(phy);
        rc = m2sdr_wide_bandwidth_verify(dev);
        if (rc == M2SDR_ERR_OK) {
            M2SDR_LOGF("Wide-bandwidth interface verified aligned (try %d).\n", try_);
            /* RX quadrature tracking loop gain: at the driver default
             * (K exp 0x15) the loop is effectively inert in this mode and
             * the image rejection settles at ~23-28dBc - a direct 3.5-7%
             * EVM floor. K exp 0x0a converges to ~40-43dBc (measured;
             * stable from 0x02 to 0x0a). M2SDR_QEC_KEXP overrides. */
            {
                const char *s = getenv("M2SDR_QEC_KEXP");
                uint8_t kexp = s ? (uint8_t)strtoul(s, NULL, 0) & 0x1F : 0x0a;

                ad9361_spi_write(phy->spi, REG_CALIBRATION_CONFIG_2,
                    CALIBRATION_CONFIG2_DFLT | K_EXP_PHASE(kexp));
                ad9361_spi_write(phy->spi, REG_CALIBRATION_CONFIG_3,
                    PREVENT_POS_LOOP_GAIN | K_EXP_AMPLITUDE(kexp));
            }
            /* RFPLL loop peaking: the LUT charge-pump current leaves a
             * ~5dB skirt bump at 1-3MHz offsets (measured); reducing it to
             * ~30% removes the peaking on both synthesizers and the EVM
             * plateaus there (RX 6.8 -> 6.2%, TX 7.3 -> 5.1% on a 50MHz TM;
             * lower values measure no further gain and erode loop margin).
             * M2SDR_RFPLL_CP_PERCENT overrides (100 = LUT value). */
            {
                const char *s = getenv("M2SDR_RFPLL_CP_PERCENT");
                unsigned pct = s ? (unsigned)strtoul(s, NULL, 0) : 30;
                static const uint16_t cp_regs[2] = {
                    REG_RX_CP_CURRENT, REG_TX_CP_CURRENT
                };
                unsigned i;

                for (i = 0; i < 2; i++) {
                    uint8_t cp = ad9361_spi_read(phy->spi, cp_regs[i]);
                    uint8_t icp = cp & CHARGE_PUMP_CURRENT(~0);
                    uint8_t nicp = (uint8_t)((icp * pct + 50) / 100);

                    if (nicp < 1)
                        nicp = 1;
                    if (nicp > CHARGE_PUMP_CURRENT(~0))
                        nicp = CHARGE_PUMP_CURRENT(~0);
                    ad9361_spi_write(phy->spi, cp_regs[i],
                        (cp & ~CHARGE_PUMP_CURRENT(~0)) | nicp);
                }
            }
            return M2SDR_ERR_OK;
        }
    }
    M2SDR_LOGF("Wide-bandwidth interface failed to align.\n");
    return rc;
}

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

#ifdef USE_LITEETH
    if (m2sdr_rf_init_timeout_expired()) {
        tls_rf_init_timed_out = true;
        M2SDR_LOGF("AD9361 SPI transaction aborted after %d ms RF init timeout.\n",
            M2SDR_LITEETH_RF_INIT_TIMEOUT_MS);
        return -ETIMEDOUT;
    }
#endif

    conn = m2sdr_conn(tls_rf_dev);

    /* The imported AD9361 driver only uses the 2-byte-address/1-byte-read and
     * 2-byte-address+1-byte-write transactions on this platform. */
    if (n_tx == 2 && n_rx == 1) {
        uint16_t reg = (txbuf[0] << 8) | txbuf[1];
        if (!m2sdr_ad9361_spi_read_checked(conn, reg, &rxbuf[0])) {
            M2SDR_LOGF("AD9361 SPI read 0x%03x failed.\n", reg);
            return -1;
        }
    } else if (n_tx == 3 && n_rx == 0) {
        uint16_t reg = (txbuf[0] << 8) | txbuf[1];
        if (!m2sdr_ad9361_spi_write_checked(conn, reg, txbuf[2])) {
            M2SDR_LOGF("AD9361 SPI write 0x%03x failed.\n", reg);
            return -1;
        }
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
    cfg->refclk_ppm        = 0;
    cfg->tx_freq           = DEFAULT_TX_FREQ;
    cfg->rx_freq           = DEFAULT_RX_FREQ;
    cfg->tx_att            = DEFAULT_TX_ATT;
    cfg->rx_gain1          = DEFAULT_RX_GAIN;
    cfg->rx_gain2          = DEFAULT_RX_GAIN;
    cfg->program_rx_gains      = true;
    cfg->program_rx_gain_modes = false;
    cfg->rx_gain_mode1     = M2SDR_RX_GAIN_MODE_MANUAL;
    cfg->rx_gain_mode2     = M2SDR_RX_GAIN_MODE_MANUAL;
    cfg->program_agc_pin   = false;
    cfg->agc_pin_enable    = false;
    cfg->loopback          = DEFAULT_LOOPBACK;
    cfg->bist_tone_freq    = DEFAULT_BIST_TONE_FREQ;
    cfg->bist_tx_tone      = false;
    cfg->bist_rx_tone      = false;
    cfg->bist_prbs         = false;
    cfg->calibrate_interface_delay = false;
    cfg->enable_8bit_mode  = false;
    cfg->sample_format      = M2SDR_FORMAT_SC16_Q11;
    cfg->channel_layout    = M2SDR_CHANNEL_LAYOUT_2T2R;
    cfg->clock_source      = M2SDR_CLOCK_SOURCE_INTERNAL;
    cfg->chan_mode         = NULL;
    cfg->sync_mode         = NULL;
}

static int m2sdr_normalize_config(const struct m2sdr_config *cfg,
                                  struct m2sdr_config *normalized)
{
    enum m2sdr_clock_source clock_source;
    enum m2sdr_channel_layout channel_layout;

    if (!cfg || !normalized)
        return M2SDR_ERR_INVAL;

    if (m2sdr_clock_source_from_config(cfg, &clock_source) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;
    if (m2sdr_channel_layout_from_config(cfg, &channel_layout) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;

    *normalized = *cfg;
    normalized->clock_source = clock_source;
    normalized->channel_layout = channel_layout;
    normalized->chan_mode = NULL;
    normalized->sync_mode = NULL;
    return M2SDR_ERR_OK;
}

static bool m2sdr_configs_equal(const struct m2sdr_config *a,
                                const struct m2sdr_config *b)
{
    return a->sample_rate == b->sample_rate &&
           a->bandwidth == b->bandwidth &&
           a->refclk_freq == b->refclk_freq &&
           a->refclk_ppm == b->refclk_ppm &&
           a->tx_freq == b->tx_freq &&
           a->rx_freq == b->rx_freq &&
           a->tx_att == b->tx_att &&
           a->rx_gain1 == b->rx_gain1 &&
           a->rx_gain2 == b->rx_gain2 &&
           a->program_rx_gains == b->program_rx_gains &&
           a->program_rx_gain_modes == b->program_rx_gain_modes &&
           a->rx_gain_mode1 == b->rx_gain_mode1 &&
           a->rx_gain_mode2 == b->rx_gain_mode2 &&
           a->program_agc_pin == b->program_agc_pin &&
           a->agc_pin_enable == b->agc_pin_enable &&
           a->loopback == b->loopback &&
           a->bist_tx_tone == b->bist_tx_tone &&
           a->bist_rx_tone == b->bist_rx_tone &&
           a->bist_prbs == b->bist_prbs &&
           a->calibrate_interface_delay == b->calibrate_interface_delay &&
           a->bist_tone_freq == b->bist_tone_freq &&
           a->enable_8bit_mode == b->enable_8bit_mode &&
           a->sample_format == b->sample_format &&
           a->channel_layout == b->channel_layout &&
           a->clock_source == b->clock_source;
}

static void m2sdr_store_applied_config(struct m2sdr_dev *dev,
                                       const struct m2sdr_config *cfg)
{
    struct m2sdr_config normalized;

    if (!dev || m2sdr_normalize_config(cfg, &normalized) != M2SDR_ERR_OK)
        return;

    dev->rf_last_config = normalized;
    dev->rf_last_config_valid = 1;
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
    AD9361_InitParam *init_param = m2sdr_dev_init_param(dev);
    if (!init_param)
        return M2SDR_ERR_NO_MEM;
    init_param->reference_clk_rate          = cfg->refclk_freq;
    init_param->gpio_resetb                 = AD9361_GPIO_RESET_PIN;
    init_param->gpio_sync                   = -1;
    init_param->gpio_cal_sw1                = -1;
    init_param->gpio_cal_sw2                = -1;
    init_param->rx_synthesizer_frequency_hz = cfg->rx_freq;
    init_param->tx_synthesizer_frequency_hz = cfg->tx_freq;
    init_param->rf_rx_bandwidth_hz          = (uint32_t)cfg->bandwidth;
    init_param->rf_tx_bandwidth_hz          = (uint32_t)cfg->bandwidth;

    {
        /* M2SDR_RF_CHANNEL=2 selects the second physical RF port pair
         * (RX2/TX2) in 1T1R; the default is RX1/TX1. */
        const char *s = getenv("M2SDR_RF_CHANNEL");
        unsigned ch = (s != NULL && strtoul(s, NULL, 0) == 2) ? 1 : 0;

        rc = m2sdr_apply_channel_layout(dev, init_param, channel_layout, ch, ch);
    }
    if (rc != M2SDR_ERR_OK)
        return rc;
#ifdef USE_LITEETH
    if (dev->transport == M2SDR_TRANSPORT_LITEETH)
        m2sdr_start_rf_init_timeout();
#endif
    rc = ad9361_init(&dev->ad9361_phy, init_param, 1);
#ifdef USE_LITEETH
    if (dev->transport == M2SDR_TRANSPORT_LITEETH && tls_rf_init_timed_out) {
        m2sdr_clear_rf_init_timeout();
        return M2SDR_ERR_TIMEOUT;
    }
    if (dev->transport == M2SDR_TRANSPORT_LITEETH)
        m2sdr_clear_rf_init_timeout();
#endif
    rc = m2sdr_from_ad9361_rc(rc);
    if (rc != M2SDR_ERR_OK)
        return m2sdr_transport_error(dev);
    phy = m2sdr_current_phy(dev);
    if (!phy)
        return M2SDR_ERR_STATE;

    dev->rf_channel_layout = channel_layout;
    dev->rf_channel_layout_valid = 1;
    /* Wide-bandwidth (2x data port) is selected by the requested rate. */
    dev->rf_oversample_enabled = (cfg->sample_rate > 61440000) ? 1 : 0;

    rc = m2sdr_configure_samplerate(phy, cfg, channel_layout);
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

    rc = m2sdr_configure_gains(phy, cfg, channel_layout);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_configure_modes(dev, phy, cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (cfg->calibrate_interface_delay) {
        rc = m2sdr_calibrate_interface_delay(dev, phy);
        if (rc != M2SDR_ERR_OK)
            return rc;
    }
    rc = m2sdr_run_bist(cfg, dev, phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    /* Applied last: above the AD9361 clock-chain limit the chip ran at half
     * rate so far; now switch the data port to 2x and open the analog
     * passband to ~100 MHz (wide-bandwidth mode), verified and retried in
     * place until the doubled-rate interface comes up aligned. */
    if (cfg->sample_rate > 61440000) {
        uint32_t ad9361_rate;

        M2SDR_LOGF("Applying wide-bandwidth mode (AD9361 oversampling + BBF widening).\n");
        rc = m2sdr_stream_to_ad9361_sample_rate(cfg->sample_rate, true, &ad9361_rate);
        if (rc != M2SDR_ERR_OK)
            return rc;
        rc = m2sdr_wide_bandwidth_bringup(dev, phy, ad9361_rate);
        if (rc != M2SDR_ERR_OK)
            return rc;
    }

    m2sdr_store_applied_config(dev, cfg);
    return M2SDR_ERR_OK;
}

int m2sdr_apply_config_if_needed(struct m2sdr_dev *dev,
                                 const struct m2sdr_config *cfg)
{
    struct m2sdr_config normalized;
    int rc;

    if (!dev || !cfg)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_validate_config_values(cfg);
    if (rc != M2SDR_ERR_OK)
        return rc;
    rc = m2sdr_normalize_config(cfg, &normalized);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (dev->rf_last_config_valid &&
        m2sdr_configs_equal(&dev->rf_last_config, &normalized))
        return M2SDR_ERR_OK;

    if (dev->ad9361_phy)
        return M2SDR_ERR_STATE;

    return m2sdr_apply_config(dev, cfg);
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
    uint32_t actual_rate;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (rate <= 0 || rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    /* Rates above the AD9361 clock-chain limit select the wide-bandwidth
     * mode (see ad9361_enable_oversampling()). Callers pass the STREAM rate;
     * the halving is handled here. */
    {
        bool wide = rate > 61440000;

        rc = m2sdr_stream_to_ad9361_sample_rate(rate, wide, &actual_rate);
        if (rc != M2SDR_ERR_OK)
            return rc;

        if (wide) {
            rc = m2sdr_wide_bandwidth_bringup(dev, phy, actual_rate);
            if (rc != M2SDR_ERR_OK)
                return rc;
        } else {
            if (m2sdr_from_ad9361_rc(ad9361_set_tx_sampling_freq(phy, actual_rate)) != M2SDR_ERR_OK)
                return M2SDR_ERR_IO;
            if (m2sdr_from_ad9361_rc(ad9361_set_rx_sampling_freq(phy, actual_rate)) != M2SDR_ERR_OK)
                return M2SDR_ERR_IO;
        }

        dev->rf_oversample_enabled = wide ? 1 : 0;
    }
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

/* Trim the SI5351 PLL to compensate a measured reference clock error. */
int m2sdr_set_refclk_ppm(struct m2sdr_dev *dev, double ppm)
{
#ifdef CSR_SI5351_BASE
    const uint8_t (*si5351_config)[2];
    size_t si5351_length;
    void *conn;

    if (!dev)
        return M2SDR_ERR_INVAL;
    /* Written to also reject NaN. */
    if (!(ppm >= -M2SDR_REFCLK_PPM_MAX && ppm <= M2SDR_REFCLK_PPM_MAX))
        return M2SDR_ERR_RANGE;
    /* The nominal SI5351 table is recovered from the last applied RF config;
     * without it the active clock topology is unknown. */
    if (!dev->rf_last_config_valid)
        return M2SDR_ERR_STATE;

    conn = m2sdr_conn(dev);
    if (!m2sdr_si5351_i2c_check_litei2c(conn))
        return M2SDR_ERR_UNSUPPORTED;

    m2sdr_si5351_select_config(dev->rf_last_config.clock_source,
                               dev->rf_last_config.refclk_freq,
                               &si5351_config, &si5351_length);
    M2SDR_LOGF("Trimming SI5351 PLL by %.3f ppm.\n", ppm);
    if (!m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        si5351_config, si5351_length, ppm))
        return m2sdr_transport_error(dev);

    dev->rf_last_config.refclk_ppm = ppm;
    return M2SDR_ERR_OK;
#else
    (void)dev;
    (void)ppm;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_set_channel_mode(struct m2sdr_dev *dev, unsigned channel_count,
                           unsigned rx_channel, unsigned tx_channel)
{
    struct ad9361_rf_phy *phy;
    struct ad9361_rf_phy *new_phy = NULL;
    enum m2sdr_channel_layout channel_layout;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (channel_count != 1 && channel_count != 2)
        return M2SDR_ERR_RANGE;
    if (rx_channel > 1 || tx_channel > 1)
        return M2SDR_ERR_RANGE;

    /* The device must have completed RF bring-up once. */
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    /* Prefer the init parameters stored at bring-up (reference clock, SPI
     * id); fall back to the built-in defaults for integrations that
     * initialized the RFIC without storing them. */
    AD9361_InitParam *init_param = (AD9361_InitParam *)dev->rf_init_param;
    if (!init_param) {
        M2SDR_LOGF("No stored AD9361 init parameters; using built-in defaults for the layout switch.\n");
        init_param = m2sdr_dev_init_param(dev);
        if (!init_param)
            return M2SDR_ERR_NO_MEM;
    }

    channel_layout = channel_count == 1 ?
        M2SDR_CHANNEL_LAYOUT_1T1R : M2SDR_CHANNEL_LAYOUT_2T2R;

    rc = m2sdr_apply_channel_layout(dev, init_param, channel_layout,
                                    rx_channel, tx_channel);
    if (rc != M2SDR_ERR_OK)
        return rc;

    /* Re-initialize the RFIC from the stored parameters so the runtime
     * layout switch goes through exactly the same configuration code as the
     * init-time path (m2sdr_apply_config), instead of the partial
     * ad9361_set_no_ch_mode dance. Use a local copy with the reset GPIO
     * masked: a hard reset mid-session makes the subsequent setup time out,
     * and the SPI-driven setup reconfigures everything on its own. The
     * previous phy instance is leaked deliberately: the no-OS ADI driver has
     * no remove/cleanup entry point and layout switches are rare, one-shot
     * events. */
    AD9361_InitParam reinit_param = *init_param;
    reinit_param.gpio_resetb = -1;
    rc = m2sdr_from_ad9361_rc(ad9361_init(&new_phy, &reinit_param, 1));
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!new_phy)
        return M2SDR_ERR_IO;

    dev->ad9361_phy = new_phy;
    tls_rf_dev = dev;

    dev->rf_channel_layout = channel_layout;
    dev->rf_channel_layout_valid = 1;

    return M2SDR_ERR_OK;
}

/* Return the currently bound AD9361 phy handle (NULL when uninitialized).
 * Callers caching the handle must refresh it after m2sdr_set_channel_mode(). */
void *m2sdr_rf_phy(struct m2sdr_dev *dev)
{
    return dev ? dev->ad9361_phy : NULL;
}

/* Read back the stream sample rate (the rate the host observes), applying the
 * inverse of the conversions used by m2sdr_set_sample_rate(). */
int m2sdr_get_sample_rate(struct m2sdr_dev *dev, int64_t *rate)
{
    struct ad9361_rf_phy *phy;
    uint32_t hw_rate = 0;
    int64_t mult = 1;
    int rc;

    if (!dev || !rate)
        return M2SDR_ERR_INVAL;
    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_get_rx_sampling_freq(phy, &hw_rate)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    if (dev->rf_oversample_enabled)
        mult *= 2;

    *rate = (int64_t)hw_rate * mult;
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
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 0, RF_GAIN_MGC)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, 1, RF_GAIN_MGC)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 0, gain)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
        if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, 1, gain)) != M2SDR_ERR_OK)
            return M2SDR_ERR_IO;
    }

    return M2SDR_ERR_OK;
}

int m2sdr_set_rx_gain_mode_all(struct m2sdr_dev *dev, enum m2sdr_rx_gain_mode mode)
{
    int rc;

    rc = m2sdr_set_rx_gain_mode(dev, 0, mode);
    if (rc != M2SDR_ERR_OK)
        return rc;
    return m2sdr_set_rx_gain_mode(dev, 1, mode);
}

int m2sdr_set_rx_gain_mode(struct m2sdr_dev *dev, unsigned channel,
                           enum m2sdr_rx_gain_mode mode)
{
    struct ad9361_rf_phy *phy;
    uint8_t ad9361_mode;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (channel > 1)
        return M2SDR_ERR_RANGE;

    rc = m2sdr_to_ad9361_gain_mode(mode, &ad9361_mode);
    if (rc != M2SDR_ERR_OK)
        return rc;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, channel, ad9361_mode)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
}

int m2sdr_get_rx_gain_mode(struct m2sdr_dev *dev, unsigned channel,
                           enum m2sdr_rx_gain_mode *mode)
{
    struct ad9361_rf_phy *phy;
    uint8_t ad9361_mode = RF_GAIN_MGC;
    int rc;

    if (!dev || !mode)
        return M2SDR_ERR_INVAL;
    if (channel > 1)
        return M2SDR_ERR_RANGE;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_get_rx_gain_control_mode(phy, channel, &ad9361_mode)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    return m2sdr_from_ad9361_gain_mode(ad9361_mode, mode);
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

    if (m2sdr_from_ad9361_rc(ad9361_set_rx_gain_control_mode(phy, channel, RF_GAIN_MGC)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (m2sdr_from_ad9361_rc(ad9361_set_rx_rf_gain(phy, channel, gain)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
}

int m2sdr_set_tx_att(struct m2sdr_dev *dev, int64_t attenuation_db)
{
    return m2sdr_set_gain(dev, M2SDR_TX, attenuation_db);
}

int m2sdr_set_rfic_loopback(struct m2sdr_dev *dev, uint8_t mode)
{
    struct ad9361_rf_phy *phy;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (mode > 2)
        return M2SDR_ERR_RANGE;

    rc = m2sdr_require_phy(dev, &phy);
    if (rc != M2SDR_ERR_OK)
        return rc;

    if (m2sdr_from_ad9361_rc(ad9361_bist_loopback(phy, mode)) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
}

int m2sdr_set_fpga_prbs_tx(struct m2sdr_dev *dev, bool enable)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

    return m2sdr_write_prbs_tx_ctrl(dev,
        enable ? (1u << CSR_AD9361_PRBS_TX_ENABLE_OFFSET) : 0);
}

int m2sdr_get_fpga_prbs_rx_synced(struct m2sdr_dev *dev, bool *synced)
{
    return m2sdr_read_prbs_synced(dev, synced);
}
