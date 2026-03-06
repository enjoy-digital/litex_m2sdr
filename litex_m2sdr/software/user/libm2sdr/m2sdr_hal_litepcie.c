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

#ifdef USE_LITEPCIE
#include "litepcie_helpers.h"

/* HAL */
/*-----*/

int m2sdr_hal_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    if (!dev || !val || dev->fd < 0)
        return M2SDR_ERR_INVAL;
    /* Keep the HAL intentionally thin so transport-neutral code pays almost no
     * abstraction cost over the original direct helper calls. */
    *val = litepcie_readl(dev->fd, addr);
    return M2SDR_ERR_OK;
}

int m2sdr_hal_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (!dev || dev->fd < 0)
        return M2SDR_ERR_INVAL;
    litepcie_writel(dev->fd, addr, val);
    return M2SDR_ERR_OK;
}

#endif /* USE_LITEPCIE */
