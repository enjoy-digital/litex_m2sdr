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

/* HAL */
/*-----*/

int m2sdr_hal_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    if (!dev || !val || !dev->eb)
        return M2SDR_ERR_INVAL;
    *val = eb_read32(dev->eb, addr);
    return M2SDR_ERR_OK;
}

int m2sdr_hal_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (!dev || !dev->eb)
        return M2SDR_ERR_INVAL;
    eb_write32(dev->eb, val, addr);
    return M2SDR_ERR_OK;
}

#endif /* USE_LITEETH */
