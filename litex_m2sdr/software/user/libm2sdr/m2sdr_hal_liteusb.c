/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

/* Includes */
/*----------*/

#include <libusb.h>

#include "m2sdr_internal.h"

static int m2sdr_from_usb_error(int err)
{
    switch (err) {
    case 0:
        return M2SDR_ERR_OK;
    case LIBUSB_ERROR_TIMEOUT:
    case LIBUSB_ERROR_INTERRUPTED:
        return M2SDR_ERR_TIMEOUT;
    case LIBUSB_ERROR_INVALID_PARAM:
        return M2SDR_ERR_INVAL;
    default:
        return M2SDR_ERR_IO;
    }
}

/* HAL */
/*-----*/

/* CSR accesses go through the EP0 Wishbone bridge (one Control transfer per access/burst of up
 * to 128 words); the lock keeps the CSR sequences (SPI/I2C) of concurrent threads apart. */

static int m2sdr_liteusb_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    int err;

    if (!dev || !val || !dev->usb)
        return M2SDR_ERR_INVAL;
    pthread_mutex_lock(&dev->reg_lock);
    err = liteusb_read32(dev->usb, addr, val);
    pthread_mutex_unlock(&dev->reg_lock);
    return m2sdr_from_usb_error(err);
}

static int m2sdr_liteusb_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    int err;

    if (!dev || !dev->usb)
        return M2SDR_ERR_INVAL;
    pthread_mutex_lock(&dev->reg_lock);
    err = liteusb_write32(dev->usb, addr, val);
    pthread_mutex_unlock(&dev->reg_lock);
    return m2sdr_from_usb_error(err);
}

static int m2sdr_liteusb_readl_bulk(struct m2sdr_dev *dev, uint32_t addr, uint32_t *vals, size_t count)
{
    int err;

    if (!dev || !vals || !dev->usb)
        return M2SDR_ERR_INVAL;
    pthread_mutex_lock(&dev->reg_lock);
    err = liteusb_read_burst(dev->usb, addr, vals, count);
    pthread_mutex_unlock(&dev->reg_lock);
    return m2sdr_from_usb_error(err);
}

static int m2sdr_liteusb_writel_bulk(struct m2sdr_dev *dev, uint32_t addr, const uint32_t *vals, size_t count)
{
    int err;

    if (!dev || !vals || !dev->usb)
        return M2SDR_ERR_INVAL;
    pthread_mutex_lock(&dev->reg_lock);
    err = liteusb_write_burst(dev->usb, addr, vals, count);
    pthread_mutex_unlock(&dev->reg_lock);
    return m2sdr_from_usb_error(err);
}

const struct m2sdr_backend_ops m2sdr_liteusb_backend_ops = {
    .readl       = m2sdr_liteusb_readl,
    .writel      = m2sdr_liteusb_writel,
    .readl_bulk  = m2sdr_liteusb_readl_bulk,
    .writel_bulk = m2sdr_liteusb_writel_bulk,
};
