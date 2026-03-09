/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_INTERNAL_H
#define M2SDR_INTERNAL_H

/* Includes */
/*----------*/

#include <stdint.h>

#include "m2sdr.h"
#include "m2sdr_rfic_iface.h"

struct ad9361_rf_phy;

#ifdef USE_LITEPCIE
#include "liblitepcie.h"
#endif

#ifdef USE_LITEETH
#include "etherbone.h"
#include "liteeth_udp.h"
#endif

enum m2sdr_transport {
    M2SDR_TRANSPORT_LITEPCIE = 0,
    M2SDR_TRANSPORT_LITEETH  = 1,
};

/* Internal device object shared by the transport, stream, and RF layers. */
struct m2sdr_dev {
    enum m2sdr_transport transport;

#ifdef USE_LITEPCIE
    int fd;
    char device_path[M2SDR_DEVICE_STR_MAX];
    struct litepcie_dma_ctrl rx_dma;
    struct litepcie_dma_ctrl tx_dma;
#endif

#ifdef USE_LITEETH
    struct eb_connection *eb;
    char eth_ip[64];
    uint16_t eth_port;
    int udp_inited;
    struct liteeth_udp_ctrl udp;
#endif

    int rx_configured;
    int tx_configured;
    int rx_header_enable;
    int rx_strip_header;
    int tx_header_enable;
    enum m2sdr_format rx_format;
    enum m2sdr_format tx_format;
    uint8_t zero_copy;
    unsigned rx_buffer_size;
    unsigned tx_buffer_size;
    unsigned rx_timeout_ms;
    unsigned tx_timeout_ms;
    /* RFIC backend selected for this device instance. The backend owns rfic_ctx
     * and may also stash vendor-driver handles in the tail fields below. */
    const struct m2sdr_rfic_ops *rfic_ops;
    void *rfic_ctx;
    enum m2sdr_rfic_kind rfic_kind;
    unsigned iq_bits;
    /* Legacy AD9361 driver handle kept here until a future backend-neutral
     * session object replaces the current vendor-driver ownership model. */
    struct ad9361_rf_phy *ad9361_phy;
};

int m2sdr_hal_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
int m2sdr_hal_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);
int m2sdr_rfic_configure_stream_channels(struct m2sdr_dev *dev,
                                         size_t rx_count, const size_t *rx_channels,
                                         size_t tx_count, const size_t *tx_channels);

int m2sdr_test_parse_identifier(const char *id, uint16_t *port_out);

#endif /* M2SDR_INTERNAL_H */
