/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR RF API dispatch layer
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "m2sdr_config.h"
#include "m2sdr_internal.h"
#include "m2sdr_rfic_iface.h"

static int m2sdr_require_backend(struct m2sdr_dev *dev)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    return m2sdr_rfic_ensure(dev);
}

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
    cfg->tx_gain           = DEFAULT_TX_GAIN;
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

int m2sdr_rf_bind(struct m2sdr_dev *dev, void *phy)
{
    int rc;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->bind)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->bind(dev, dev->rfic_ctx, phy);
}

int m2sdr_apply_config(struct m2sdr_dev *dev, const struct m2sdr_config *cfg)
{
    int rc;

    if (!dev || !cfg)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->apply_config)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->apply_config(dev, dev->rfic_ctx, cfg);
}

int m2sdr_set_frequency(struct m2sdr_dev *dev, enum m2sdr_direction direction, uint64_t freq)
{
    int rc;

    if (!dev || freq == 0)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->set_frequency)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->set_frequency(dev, dev->rfic_ctx, direction, freq);
}

int m2sdr_set_sample_rate(struct m2sdr_dev *dev, int64_t rate)
{
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (rate <= 0 || rate > UINT32_MAX)
        return M2SDR_ERR_RANGE;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->set_sample_rate)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->set_sample_rate(dev, dev->rfic_ctx, rate);
}

int m2sdr_set_bandwidth(struct m2sdr_dev *dev, int64_t bw)
{
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (bw <= 0 || bw > UINT32_MAX)
        return M2SDR_ERR_RANGE;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->set_bandwidth)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->set_bandwidth(dev, dev->rfic_ctx, bw);
}

int m2sdr_set_gain(struct m2sdr_dev *dev, enum m2sdr_direction direction, int64_t gain)
{
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_TX && direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->set_gain)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->set_gain(dev, dev->rfic_ctx, direction, gain);
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

int m2sdr_get_rfic_name(struct m2sdr_dev *dev, char *buf, size_t len)
{
    int rc;

    if (!dev || !buf || len == 0)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->name)
        return M2SDR_ERR_UNSUPPORTED;

    if (strnlen(dev->rfic_ops->name, len) >= len)
        return M2SDR_ERR_RANGE;
    snprintf(buf, len, "%s", dev->rfic_ops->name);
    return M2SDR_ERR_OK;
}

int m2sdr_get_rfic_caps(struct m2sdr_dev *dev, struct m2sdr_rfic_caps *caps)
{
    int rc;

    if (!dev || !caps)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->get_caps)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->get_caps(dev, dev->rfic_ctx, caps);
}

int m2sdr_set_property(struct m2sdr_dev *dev, const char *key, const char *value)
{
    int rc;

    if (!dev || !key || !value)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->set_property)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->set_property(dev, dev->rfic_ctx, key, value);
}

int m2sdr_get_property(struct m2sdr_dev *dev, const char *key, char *value, size_t value_len)
{
    int rc;

    if (!dev || !key || !value || value_len == 0)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_require_backend(dev);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (!dev->rfic_ops || !dev->rfic_ops->get_property)
        return M2SDR_ERR_UNSUPPORTED;

    return dev->rfic_ops->get_property(dev, dev->rfic_ctx, key, value, value_len);
}
