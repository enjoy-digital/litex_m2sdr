/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR RFIC backend registry
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <stdlib.h>
#include <string.h>

#include "m2sdr_internal.h"

extern const struct m2sdr_rfic_ops m2sdr_rfic_ad9361_ops;
extern const struct m2sdr_rfic_ops m2sdr_rfic_mock_ops;

static const struct m2sdr_rfic_ops *m2sdr_lookup_rfic(const char *name)
{
    /* Default to the shipped RFIC when no explicit override is provided. This
     * keeps existing deployments working while allowing alternative backends to
     * be exercised via M2SDR_RFIC without changing callers. */
    if (!name || !name[0])
        return &m2sdr_rfic_ad9361_ops;

    if (strcmp(name, "ad9361") == 0)
        return &m2sdr_rfic_ad9361_ops;
    if (strcmp(name, "mock") == 0)
        return &m2sdr_rfic_mock_ops;

    return NULL;
}

int m2sdr_rfic_attach_default(struct m2sdr_dev *dev)
{
    const struct m2sdr_rfic_ops *ops;
    const char *override;
    int rc;

    if (!dev)
        return M2SDR_ERR_INVAL;
    if (dev->rfic_ops)
        return M2SDR_ERR_OK;

    override = getenv("M2SDR_RFIC");
    ops = m2sdr_lookup_rfic(override);
    if (!ops)
        return M2SDR_ERR_UNSUPPORTED;

    rc = ops->init ? ops->init(dev, &dev->rfic_ctx) : M2SDR_ERR_OK;
    if (rc != M2SDR_ERR_OK)
        return rc;

    dev->rfic_ops = ops;
    dev->rfic_kind = ops->kind;
    return M2SDR_ERR_OK;
}

int m2sdr_rfic_ensure(struct m2sdr_dev *dev)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    if (dev->rfic_ops)
        return M2SDR_ERR_OK;
    return m2sdr_rfic_attach_default(dev);
}

void m2sdr_rfic_detach(struct m2sdr_dev *dev)
{
    if (!dev)
        return;
    /* cleanup() is backend-owned because only the backend knows whether rfic_ctx
     * also implies vendor-driver state that needs teardown or just cached policy. */
    if (dev->rfic_ops && dev->rfic_ops->cleanup)
        dev->rfic_ops->cleanup(dev, dev->rfic_ctx);
    dev->rfic_ctx = NULL;
    dev->rfic_ops = NULL;
    dev->rfic_kind = M2SDR_RFIC_KIND_UNKNOWN;
}
