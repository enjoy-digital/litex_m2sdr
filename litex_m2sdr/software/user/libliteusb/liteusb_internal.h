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
    uint32_t claimed; /* Claimed interfaces (bitmask). */
    libusb_context *ctx;
    libusb_device_handle *handle;
    struct liteusb_device_info info;
};

/* Claim/release the interface of a stream endpoint (claimed per stream: RX and TX streams can be
 * used by separate processes, EP0 CSR accesses do not need a claimed interface). */
int  liteusb_claim_endpoint(struct liteusb_connection *conn, unsigned char ep);
void liteusb_release_interface(struct liteusb_connection *conn, int interface);

#endif /* LITEUSB_INTERNAL_H */
