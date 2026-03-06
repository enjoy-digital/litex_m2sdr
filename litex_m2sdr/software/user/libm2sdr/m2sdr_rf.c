/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR RF configuration API
 */

#include "m2sdr_internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "m2sdr_config.h"
#include "m2sdr_ad9361_spi.h"
#include "m2sdr_si5351_i2c.h"

#include "platform.h"
#include "ad9361.h"
#include "ad9361_api.h"

#include "csr.h"

#define AD9361_GPIO_RESET_PIN 0

#if defined(__GNUC__) || defined(__clang__)
#define M2SDR_WEAK __attribute__((weak))
#else
#define M2SDR_WEAK
#endif

static struct m2sdr_dev *g_rf_dev;
static struct ad9361_rf_phy *ad9361_phy;

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

int M2SDR_WEAK spi_write_then_read(struct spi_device *spi,
                                   const unsigned char *txbuf, unsigned n_tx,
                                   unsigned char *rxbuf, unsigned n_rx)
{
    (void)spi;
    void *conn = m2sdr_conn(g_rf_dev);

    if (n_tx == 2 && n_rx == 1) {
        rxbuf[0] = m2sdr_ad9361_spi_read(conn, (txbuf[0] << 8) | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        m2sdr_ad9361_spi_write(conn, (txbuf[0] << 8) | txbuf[1], txbuf[2]);
    } else {
        return -1;
    }

    return 0;
}

void M2SDR_WEAK udelay(unsigned long usecs)
{
    usleep(usecs);
}

void M2SDR_WEAK mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

unsigned long M2SDR_WEAK msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

bool M2SDR_WEAK gpio_is_valid(int number)
{
    return number == AD9361_GPIO_RESET_PIN;
}

void M2SDR_WEAK gpio_set_value(unsigned gpio, int value)
{
    (void)gpio;
    (void)value;
}

void m2sdr_config_init(struct m2sdr_config *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate      = DEFAULT_SAMPLERATE;
    cfg->bandwidth        = DEFAULT_BANDWIDTH;
    cfg->refclk_freq      = DEFAULT_REFCLK_FREQ;
    cfg->tx_freq          = DEFAULT_TX_FREQ;
    cfg->rx_freq          = DEFAULT_RX_FREQ;
    cfg->tx_gain          = DEFAULT_TX_GAIN;
    cfg->rx_gain1         = DEFAULT_RX_GAIN;
    cfg->rx_gain2         = DEFAULT_RX_GAIN;
    cfg->loopback         = DEFAULT_LOOPBACK;
    cfg->bist_tone_freq   = DEFAULT_BIST_TONE_FREQ;
    cfg->bist_tx_tone     = false;
    cfg->bist_rx_tone     = false;
    cfg->bist_prbs        = false;
    cfg->enable_8bit_mode = false;
    cfg->enable_oversample= false;
    cfg->channel_layout   = M2SDR_CHANNEL_LAYOUT_2T2R;
    cfg->clock_source     = M2SDR_CLOCK_SOURCE_INTERNAL;
    cfg->chan_mode        = NULL;
    cfg->sync_mode        = NULL;
}

int m2sdr_rf_bind(struct m2sdr_dev *dev, void *phy)
{
    g_rf_dev = dev;
    ad9361_phy = (struct ad9361_rf_phy *)phy;
    if (!dev && phy)
        return M2SDR_ERR_INVAL;
    return M2SDR_ERR_OK;
}

static int m2sdr_clock_source_from_config(const struct m2sdr_config *cfg, enum m2sdr_clock_source *source)
{
    if (!cfg || !source)
        return M2SDR_ERR_INVAL;
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

static int m2sdr_channel_layout_from_config(const struct m2sdr_config *cfg, enum m2sdr_channel_layout *layout)
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

int m2sdr_apply_config(struct m2sdr_dev *dev, const struct m2sdr_config *cfg)
{
    if (!dev || !cfg)
        return M2SDR_ERR_INVAL;

    g_rf_dev = dev;
    void *conn = m2sdr_conn(dev);
    enum m2sdr_clock_source clock_source = M2SDR_CLOCK_SOURCE_INTERNAL;
    enum m2sdr_channel_layout channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;

    /* Keep the public API typed, while still honoring the legacy string
     * overrides used by older utilities. */
    if (m2sdr_clock_source_from_config(cfg, &clock_source) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;
    if (m2sdr_channel_layout_from_config(cfg, &channel_layout) != M2SDR_ERR_OK)
        return M2SDR_ERR_INVAL;

#ifdef CSR_SI5351_BASE
    /* Initialize SI5351 Clocking */
    printf("Initializing SI5351 Clocking...\n");
    if (clock_source == M2SDR_CLOCK_SOURCE_INTERNAL) {
        printf("Using internal XO as SI5351 CLKIN source...\n");
        m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
            SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET));

        if (cfg->refclk_freq == 40000000) {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_xo_40m_config,
                sizeof(si5351_xo_40m_config)/sizeof(si5351_xo_40m_config[0]));
        } else {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_xo_38p4m_config,
                sizeof(si5351_xo_38p4m_config)/sizeof(si5351_xo_38p4m_config[0]));
        }
    } else if (clock_source == M2SDR_CLOCK_SOURCE_EXTERNAL) {
        printf("Using external 10MHz as SI5351 CLKIN source...\n");
        m2sdr_reg_write(dev, CSR_SI5351_CONTROL_ADDR,
              SI5351C_VERSION               * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET) |
              SI5351C_10MHZ_CLK_IN_FROM_UFL * (1 << CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET));

        if (cfg->refclk_freq == 40000000) {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_clkin_10m_40m_config,
                sizeof(si5351_clkin_10m_40m_config)/sizeof(si5351_clkin_10m_40m_config[0]));
        } else {
            m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
                si5351_clkin_10m_38p4m_config,
                sizeof(si5351_clkin_10m_38p4m_config)/sizeof(si5351_clkin_10m_38p4m_config[0]));
        }
    } else {
        return M2SDR_ERR_INVAL;
    }
#endif

    /* Initialize AD9361 SPI */
    printf("Initializing AD9361 SPI...\n");
    m2sdr_ad9361_spi_init(conn, 1);

    /* Initialize AD9361 RFIC */
    printf("Initializing AD9361 RFIC...\n");
    default_init_param.reference_clk_rate = cfg->refclk_freq;
    default_init_param.gpio_resetb        = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync          = -1;
    default_init_param.gpio_cal_sw1       = -1;
    default_init_param.gpio_cal_sw2       = -1;

    if (channel_layout == M2SDR_CHANNEL_LAYOUT_1T1R) {
        printf("Setting Channel Mode to 1T1R.\n");
        default_init_param.two_rx_two_tx_mode_enable     = 0;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
        default_init_param.two_t_two_r_timing_enable     = 0;
        m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, 1);
    }
    if (channel_layout == M2SDR_CHANNEL_LAYOUT_2T2R) {
        printf("Setting Channel Mode to 2T2R.\n");
        default_init_param.two_rx_two_tx_mode_enable     = 1;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 1;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 1;
        default_init_param.two_t_two_r_timing_enable     = 1;
        m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, 0);
    }
    ad9361_init(&ad9361_phy, &default_init_param, 1);

    /* Configure samplerate */
    uint32_t actual_samplerate = cfg->sample_rate;
    printf("Setting TX/RX Samplerate to %f MSPS.\n", cfg->sample_rate / 1e6);
    if (cfg->enable_oversample)
        actual_samplerate /= 2;
    if (actual_samplerate < 2500000) {
        printf("Setting TX/RX FIR Interpolation/Decimation to 4 (< 2.5 Msps Samplerate).\n");
        ad9361_phy->rx_fir_dec    = 4;
        ad9361_phy->tx_fir_int    = 4;
        ad9361_phy->bypass_rx_fir = 0;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_RXFIRConfig rx_fir_cfg = rx_fir_config;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config;
        rx_fir_cfg.rx_dec = 4;
        tx_fir_cfg.tx_int = 4;
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_cfg);
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_rx_fir_en_dis(ad9361_phy, 1);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    }
    ad9361_set_tx_sampling_freq(ad9361_phy, actual_samplerate);
    ad9361_set_rx_sampling_freq(ad9361_phy, actual_samplerate);

    /* Configure bandwidth */
    printf("Setting TX/RX Bandwidth to %f MHz.\n", cfg->bandwidth / 1e6);
    ad9361_set_rx_rf_bandwidth(ad9361_phy, cfg->bandwidth);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, cfg->bandwidth);

    /* Configure frequencies */
    printf("Setting TX LO Freq to %f MHz.\n", cfg->tx_freq / 1e6);
    printf("Setting RX LO Freq to %f MHz.\n", cfg->rx_freq / 1e6);
    ad9361_set_tx_lo_freq(ad9361_phy, cfg->tx_freq);
    ad9361_set_rx_lo_freq(ad9361_phy, cfg->rx_freq);

    /* FIRs */
    ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    /* Gain */
    printf("Setting TX Gain to %ld dB.\n", (long)cfg->tx_gain);
    ad9361_set_tx_atten(ad9361_phy, -cfg->tx_gain * 1000, 1, 1, 1);
    printf("Setting RX Gain to %ld dB and %ld dB.\n", (long)cfg->rx_gain1, (long)cfg->rx_gain2);
    ad9361_set_rx_rf_gain(ad9361_phy, 0, cfg->rx_gain1);
    ad9361_set_rx_rf_gain(ad9361_phy, 1, cfg->rx_gain2);

    /* Loopback */
    printf("Setting Loopback to %d\n", cfg->loopback);
    ad9361_bist_loopback(ad9361_phy, cfg->loopback);

    /* 8-bit mode */
    if (cfg->enable_8bit_mode)
        printf("Enabling 8-bit mode.\n");
    else
        printf("Enabling 16-bit mode.\n");
    m2sdr_reg_write(dev, CSR_AD9361_BITMODE_ADDR, cfg->enable_8bit_mode ? 1 : 0);

    if (cfg->bist_tx_tone) {
        printf("BIST_TX_TONE_TEST...\n");
        ad9361_bist_tone(ad9361_phy, BIST_INJ_TX, cfg->bist_tone_freq, 0, 0x0);
    }
    if (cfg->bist_rx_tone) {
        printf("BIST_RX_TONE_TEST...\n");
        ad9361_bist_tone(ad9361_phy, BIST_INJ_RX, cfg->bist_tone_freq, 0, 0x0);
    }

    if (cfg->bist_prbs) {
        printf("BIST_PRBS TEST...\n");
        m2sdr_reg_write(dev, CSR_AD9361_PRBS_TX_ADDR, 0 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));
        ad9361_bist_prbs(ad9361_phy, BIST_INJ_RX);
    }

    if (cfg->enable_oversample)
        ad9361_enable_oversampling(ad9361_phy);

    return M2SDR_ERR_OK;
}

int m2sdr_set_frequency(struct m2sdr_dev *dev, enum m2sdr_direction direction, uint64_t freq)
{
    (void)dev;
    if (!ad9361_phy)
        return M2SDR_ERR_UNEXPECTED;
    if (direction == M2SDR_TX)
        ad9361_set_tx_lo_freq(ad9361_phy, freq);
    else
        ad9361_set_rx_lo_freq(ad9361_phy, freq);
    return M2SDR_ERR_OK;
}

int m2sdr_set_sample_rate(struct m2sdr_dev *dev, int64_t rate)
{
    (void)dev;
    if (!ad9361_phy)
        return M2SDR_ERR_UNEXPECTED;
    ad9361_set_tx_sampling_freq(ad9361_phy, (uint32_t)rate);
    ad9361_set_rx_sampling_freq(ad9361_phy, (uint32_t)rate);
    return M2SDR_ERR_OK;
}

int m2sdr_set_bandwidth(struct m2sdr_dev *dev, int64_t bw)
{
    (void)dev;
    if (!ad9361_phy)
        return M2SDR_ERR_UNEXPECTED;
    ad9361_set_rx_rf_bandwidth(ad9361_phy, bw);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, bw);
    return M2SDR_ERR_OK;
}

int m2sdr_set_gain(struct m2sdr_dev *dev, enum m2sdr_direction direction, int64_t gain)
{
    (void)dev;
    if (!ad9361_phy)
        return M2SDR_ERR_UNEXPECTED;
    if (direction == M2SDR_TX) {
        ad9361_set_tx_atten(ad9361_phy, -gain * 1000, 1, 1, 1);
    } else {
        ad9361_set_rx_rf_gain(ad9361_phy, 0, gain);
        ad9361_set_rx_rf_gain(ad9361_phy, 1, gain);
    }
    return M2SDR_ERR_OK;
}

int m2sdr_set_rx_frequency(struct m2sdr_dev *dev, uint64_t freq)
{
    return m2sdr_set_frequency(dev, M2SDR_RX, freq);
}

int m2sdr_set_tx_frequency(struct m2sdr_dev *dev, uint64_t freq)
{
    return m2sdr_set_frequency(dev, M2SDR_TX, freq);
}

int m2sdr_set_rx_gain(struct m2sdr_dev *dev, int64_t gain)
{
    return m2sdr_set_gain(dev, M2SDR_RX, gain);
}

int m2sdr_set_tx_gain(struct m2sdr_dev *dev, int64_t gain)
{
    return m2sdr_set_gain(dev, M2SDR_TX, gain);
}
