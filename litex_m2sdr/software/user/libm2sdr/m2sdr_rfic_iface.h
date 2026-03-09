/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR RFIC backend interface (internal)
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#ifndef M2SDR_RFIC_IFACE_H
#define M2SDR_RFIC_IFACE_H

#include <stddef.h>

#include "m2sdr.h"

struct m2sdr_dev;

struct m2sdr_rfic_ops {
    enum m2sdr_rfic_kind kind;
    const char *name;
    int (*init)(struct m2sdr_dev *dev, void **ctx_out);
    void (*cleanup)(struct m2sdr_dev *dev, void *ctx);
    int (*bind)(struct m2sdr_dev *dev, void *ctx, void *phy);
    int (*apply_config)(struct m2sdr_dev *dev, void *ctx, const struct m2sdr_config *cfg);
    int (*set_frequency)(struct m2sdr_dev *dev, void *ctx, enum m2sdr_direction direction, uint64_t freq);
    int (*set_sample_rate)(struct m2sdr_dev *dev, void *ctx, int64_t rate);
    int (*set_bandwidth)(struct m2sdr_dev *dev, void *ctx, int64_t bw);
    int (*set_gain)(struct m2sdr_dev *dev, void *ctx, enum m2sdr_direction direction, int64_t gain);
    int (*set_iq_bits)(struct m2sdr_dev *dev, void *ctx, unsigned bits);
    int (*get_iq_bits)(struct m2sdr_dev *dev, void *ctx, unsigned *bits);
    int (*get_caps)(struct m2sdr_dev *dev, void *ctx, struct m2sdr_rfic_caps *caps);
    int (*set_property)(struct m2sdr_dev *dev, void *ctx, const char *key, const char *value);
    int (*get_property)(struct m2sdr_dev *dev, void *ctx, const char *key, char *value, size_t value_len);
};

int m2sdr_rfic_attach_default(struct m2sdr_dev *dev);
int m2sdr_rfic_ensure(struct m2sdr_dev *dev);
void m2sdr_rfic_detach(struct m2sdr_dev *dev);

#endif /* M2SDR_RFIC_IFACE_H */
