/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR mock RFIC backend
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "m2sdr_config.h"
#include "m2sdr_internal.h"

struct m2sdr_rfic_mock_ctx {
    uint64_t freq[2];
    int64_t sample_rate;
    int64_t bandwidth;
    int64_t gain[2];
    unsigned iq_bits;
};

static int m2sdr_rfic_mock_init(struct m2sdr_dev *dev, void **ctx_out)
{
    struct m2sdr_rfic_mock_ctx *ctx;

    if (!dev || !ctx_out)
        return M2SDR_ERR_INVAL;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return M2SDR_ERR_NO_MEM;

    ctx->freq[M2SDR_RX] = DEFAULT_RX_FREQ;
    ctx->freq[M2SDR_TX] = DEFAULT_TX_FREQ;
    ctx->sample_rate = DEFAULT_SAMPLERATE;
    ctx->bandwidth = DEFAULT_BANDWIDTH;
    ctx->gain[M2SDR_RX] = DEFAULT_RX_GAIN;
    ctx->gain[M2SDR_TX] = DEFAULT_TX_GAIN;
    ctx->iq_bits = 16u;
    dev->iq_bits = 16u;
    *ctx_out = ctx;
    return M2SDR_ERR_OK;
}

static void m2sdr_rfic_mock_cleanup(struct m2sdr_dev *dev, void *ctx)
{
    (void)dev;
    free(ctx);
}

static int m2sdr_rfic_mock_bind(struct m2sdr_dev *dev, void *ctx, void *phy)
{
    (void)dev;
    (void)ctx;
    (void)phy;
    return M2SDR_ERR_UNSUPPORTED;
}

static int m2sdr_rfic_mock_apply_config(struct m2sdr_dev *dev, void *ctx_void,
                                        const struct m2sdr_config *cfg)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || !cfg)
        return M2SDR_ERR_INVAL;
    if (cfg->sample_rate <= 0 || cfg->bandwidth <= 0 || cfg->tx_freq <= 0 || cfg->rx_freq <= 0)
        return M2SDR_ERR_RANGE;

    ctx->sample_rate = cfg->sample_rate;
    ctx->bandwidth = cfg->bandwidth;
    ctx->freq[M2SDR_TX] = (uint64_t)cfg->tx_freq;
    ctx->freq[M2SDR_RX] = (uint64_t)cfg->rx_freq;
    ctx->gain[M2SDR_TX] = cfg->tx_gain;
    ctx->gain[M2SDR_RX] = cfg->rx_gain1;
    ctx->iq_bits = cfg->enable_8bit_mode ? 8u : 16u;
    dev->iq_bits = ctx->iq_bits;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_set_frequency(struct m2sdr_dev *dev, void *ctx_void,
                                         enum m2sdr_direction direction, uint64_t freq)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || freq == 0)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX && direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;

    ctx->freq[direction] = freq;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_get_frequency(struct m2sdr_dev *dev, void *ctx_void,
                                         enum m2sdr_direction direction, uint64_t *freq)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || !freq)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX && direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;

    *freq = ctx->freq[direction];
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_set_sample_rate(struct m2sdr_dev *dev, void *ctx_void, int64_t rate)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx)
        return M2SDR_ERR_INVAL;
    if (rate <= 0)
        return M2SDR_ERR_RANGE;
    ctx->sample_rate = rate;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_get_sample_rate(struct m2sdr_dev *dev, void *ctx_void, int64_t *rate)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || !rate)
        return M2SDR_ERR_INVAL;
    *rate = ctx->sample_rate;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_set_bandwidth(struct m2sdr_dev *dev, void *ctx_void, int64_t bw)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx)
        return M2SDR_ERR_INVAL;
    if (bw <= 0)
        return M2SDR_ERR_RANGE;
    ctx->bandwidth = bw;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_get_bandwidth(struct m2sdr_dev *dev, void *ctx_void, int64_t *bw)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || !bw)
        return M2SDR_ERR_INVAL;
    *bw = ctx->bandwidth;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_set_gain(struct m2sdr_dev *dev, void *ctx_void,
                                    enum m2sdr_direction direction, int64_t gain)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX && direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;
    ctx->gain[direction] = gain;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_get_gain(struct m2sdr_dev *dev, void *ctx_void,
                                    enum m2sdr_direction direction, int64_t *gain)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || !gain)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX && direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;
    *gain = ctx->gain[direction];
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_set_iq_bits(struct m2sdr_dev *dev, void *ctx_void, unsigned bits)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx)
        return M2SDR_ERR_INVAL;
    if (bits != 8u && bits != 12u && bits != 16u)
        return M2SDR_ERR_RANGE;
    ctx->iq_bits = bits;
    dev->iq_bits = bits;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_get_iq_bits(struct m2sdr_dev *dev, void *ctx_void, unsigned *bits)
{
    struct m2sdr_rfic_mock_ctx *ctx = ctx_void;

    if (!dev || !ctx || !bits)
        return M2SDR_ERR_INVAL;
    *bits = ctx->iq_bits;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_configure_stream_channels(struct m2sdr_dev *dev, void *ctx_void,
                                                     size_t rx_count, const size_t *rx_channels,
                                                     size_t tx_count, const size_t *tx_channels)
{
    (void)dev;
    (void)ctx_void;
    (void)rx_count;
    (void)rx_channels;
    (void)tx_count;
    (void)tx_channels;
    return M2SDR_ERR_UNSUPPORTED;
}

static int m2sdr_rfic_mock_get_caps(struct m2sdr_dev *dev, void *ctx_void, struct m2sdr_rfic_caps *caps)
{
    (void)dev;
    (void)ctx_void;

    if (!caps)
        return M2SDR_ERR_INVAL;

    memset(caps, 0, sizeof(*caps));
    caps->kind = M2SDR_RFIC_KIND_MOCK;
    snprintf(caps->name, sizeof(caps->name), "mock");
    caps->features = 0;
    caps->rx_channels = 2u;
    caps->tx_channels = 2u;
    caps->min_rx_frequency = 30000000LL;
    caps->max_rx_frequency = 6000000000LL;
    caps->min_tx_frequency = 30000000LL;
    caps->max_tx_frequency = 6000000000LL;
    caps->min_sample_rate = 1;
    caps->max_sample_rate = 245760000;
    caps->min_bandwidth = 1;
    caps->max_bandwidth = 200000000;
    caps->min_tx_gain = -120;
    caps->max_tx_gain = 36;
    caps->min_rx_gain = -12;
    caps->max_rx_gain = 96;
    caps->supported_iq_bits_mask = M2SDR_IQ_BITS_MASK(8) | M2SDR_IQ_BITS_MASK(12) | M2SDR_IQ_BITS_MASK(16);
    caps->native_iq_bits = 16u;
    caps->min_iq_bits = 8u;
    caps->max_iq_bits = 16u;
    return M2SDR_ERR_OK;
}

static int m2sdr_rfic_mock_set_property(struct m2sdr_dev *dev, void *ctx_void, const char *key, const char *value)
{
    (void)ctx_void;

    if (!dev || !key || !value)
        return M2SDR_ERR_INVAL;
    if (strcmp(key, "mock.iq_bits") == 0) {
        char *end = NULL;
        unsigned long bits = strtoul(value, &end, 10);
        if (!end || *end != '\0')
            return M2SDR_ERR_PARSE;
        return m2sdr_rfic_mock_set_iq_bits(dev, ctx_void, (unsigned)bits);
    }
    return M2SDR_ERR_UNSUPPORTED;
}

static int m2sdr_rfic_mock_get_property(struct m2sdr_dev *dev, void *ctx_void,
                                        const char *key, char *value, size_t value_len)
{
    unsigned bits = 0;
    int rc;

    if (!dev || !ctx_void || !key || !value || value_len == 0)
        return M2SDR_ERR_INVAL;
    if (strcmp(key, "mock.iq_bits") != 0)
        return M2SDR_ERR_UNSUPPORTED;

    rc = m2sdr_rfic_mock_get_iq_bits(dev, ctx_void, &bits);
    if (rc != M2SDR_ERR_OK)
        return rc;
    if (snprintf(value, value_len, "%u", bits) >= (int)value_len)
        return M2SDR_ERR_RANGE;
    return M2SDR_ERR_OK;
}

const struct m2sdr_rfic_ops m2sdr_rfic_mock_ops = {
    .kind            = M2SDR_RFIC_KIND_MOCK,
    .name            = "mock",
    .init            = m2sdr_rfic_mock_init,
    .cleanup         = m2sdr_rfic_mock_cleanup,
    .bind            = m2sdr_rfic_mock_bind,
    .apply_config    = m2sdr_rfic_mock_apply_config,
    .set_frequency   = m2sdr_rfic_mock_set_frequency,
    .get_frequency   = m2sdr_rfic_mock_get_frequency,
    .set_sample_rate = m2sdr_rfic_mock_set_sample_rate,
    .get_sample_rate = m2sdr_rfic_mock_get_sample_rate,
    .set_bandwidth   = m2sdr_rfic_mock_set_bandwidth,
    .get_bandwidth   = m2sdr_rfic_mock_get_bandwidth,
    .set_gain        = m2sdr_rfic_mock_set_gain,
    .get_gain        = m2sdr_rfic_mock_get_gain,
    .set_iq_bits     = m2sdr_rfic_mock_set_iq_bits,
    .get_iq_bits     = m2sdr_rfic_mock_get_iq_bits,
    .configure_stream_channels = m2sdr_rfic_mock_configure_stream_channels,
    .get_caps        = m2sdr_rfic_mock_get_caps,
    .set_property    = m2sdr_rfic_mock_set_property,
    .get_property    = m2sdr_rfic_mock_get_property,
};
