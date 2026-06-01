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

#include "m2sdr_internal.h"

static int m2sdr_from_eb_error(int err)
{
    switch (err) {
    case EB_ERR_OK:
        return M2SDR_ERR_OK;
    case EB_ERR_TIMEOUT:
    case EB_ERR_INTERRUPTED:
        return M2SDR_ERR_TIMEOUT;
    default:
        return M2SDR_ERR_IO;
    }
}

/* HAL */
/*-----*/

static int m2sdr_liteeth_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    int err;

    if (!dev || !val || !dev->eb)
        return M2SDR_ERR_INVAL;
    /* Etherbone already presents CSR access as 32-bit register transactions,
     * so the HAL is just a validity check plus the backend call. */
    err = eb_read32_checked(dev->eb, addr, val);
    return m2sdr_from_eb_error(err);
}

static int m2sdr_liteeth_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    int err;

    if (!dev || !dev->eb)
        return M2SDR_ERR_INVAL;
    err = eb_write32_checked(dev->eb, val, addr);
    return m2sdr_from_eb_error(err);
}

static int m2sdr_liteeth_readl_bulk(struct m2sdr_dev *dev, uint32_t addr, uint32_t *vals, size_t count)
{
    int err;

    if (!dev || !vals || !dev->eb)
        return M2SDR_ERR_INVAL;
    err = eb_read32_bulk_checked(dev->eb, addr, vals, count);
    return m2sdr_from_eb_error(err);
}

static int m2sdr_liteeth_writel_bulk(struct m2sdr_dev *dev, uint32_t addr, const uint32_t *vals, size_t count)
{
    int err;

    if (!dev || !vals || !dev->eb)
        return M2SDR_ERR_INVAL;
    err = eb_write32_bulk_checked(dev->eb, addr, vals, count);
    return m2sdr_from_eb_error(err);
}

const struct m2sdr_backend_ops m2sdr_liteeth_backend_ops = {
    .readl       = m2sdr_liteeth_readl,
    .writel      = m2sdr_liteeth_writel,
    .readl_bulk  = m2sdr_liteeth_readl_bulk,
    .writel_bulk = m2sdr_liteeth_writel_bulk,
};
