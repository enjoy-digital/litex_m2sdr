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

#ifdef USE_LITEETH

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

int m2sdr_hal_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    int err;

    if (!dev || !val || !dev->eb)
        return M2SDR_ERR_INVAL;
    /* Etherbone already presents CSR access as 32-bit register transactions,
     * so the HAL is just a validity check plus the backend call. */
    err = eb_read32_checked(dev->eb, addr, val);
    return m2sdr_from_eb_error(err);
}

int m2sdr_hal_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    int err;

    if (!dev || !dev->eb)
        return M2SDR_ERR_INVAL;
    err = eb_write32_checked(dev->eb, val, addr);
    return m2sdr_from_eb_error(err);
}

#endif /* USE_LITEETH */
