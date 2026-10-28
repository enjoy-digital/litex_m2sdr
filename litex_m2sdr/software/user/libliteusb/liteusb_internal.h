/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteUSB lib (USB3-PIPE device: EP0 Wishbone bridge + Bulk streams, libusb-1.0).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef LITEUSB_INTERNAL_H
#define LITEUSB_INTERNAL_H

#include <libusb.h>

#include "liteusb.h"

struct liteusb_connection {
    int magic; /* LITEUSB_CONNECTION_MAGIC, must stay first (see liteusb_is_connection). */
    int last_error;
    int claimed;
    libusb_context *ctx;
    libusb_device_handle *handle;
    struct liteusb_device_info info;
};

#endif /* LITEUSB_INTERNAL_H */
