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
#include "m2sdr_platform.h"
#ifdef USE_LITEPCIE
#include "liblitepcie.h"
#define M2SDR_HAVE_LITEPCIE 1
#else
#define M2SDR_HAVE_LITEPCIE 0
struct litepcie_dma_ctrl {
    int unused;
};
#endif

#ifdef USE_LITEETH
#include "etherbone.h"
#include "liteeth_udp.h"
#define M2SDR_HAVE_LITEETH 1
#else
#define M2SDR_HAVE_LITEETH 0
struct eb_connection;
struct liteeth_udp_ctrl {
    int unused;
};
#endif

struct ad9361_rf_phy;

enum m2sdr_transport {
    M2SDR_TRANSPORT_LITEPCIE = 0,
    M2SDR_TRANSPORT_LITEETH  = 1,
};

struct m2sdr_backend_ops {
    int (*readl)(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
    int (*writel)(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);
};

/* Internal device object shared by the transport, stream, and RF layers. */
struct m2sdr_dev {
    enum m2sdr_transport transport;
    const struct m2sdr_backend_ops *ops;

    int fd;
    char device_path[M2SDR_DEVICE_STR_MAX];
    struct litepcie_dma_ctrl rx_dma;
    struct litepcie_dma_ctrl tx_dma;

    struct eb_connection *eb;
    char eth_ip[64];
    uint16_t eth_port;
    int udp_inited;
    struct liteeth_udp_ctrl udp;
    struct m2sdr_liteeth_rx_stream_config liteeth_rx_config;
    int liteeth_rx_config_valid;
    int liteeth_rx_timeout_recovery_armed;
    int liteeth_rx_timeout_recovery_disabled;

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
    int64_t rx_user_count;
    int64_t rx_release_count;
    int64_t tx_user_count;
    int64_t tx_submit_count;
    struct ad9361_rf_phy *ad9361_phy;
};

extern const struct m2sdr_backend_ops m2sdr_litepcie_backend_ops;
extern const struct m2sdr_backend_ops m2sdr_liteeth_backend_ops;

void m2sdr_stream_cleanup(struct m2sdr_dev *dev);

int m2sdr_test_parse_identifier(const char *id, uint16_t *port_out);

#endif /* M2SDR_INTERNAL_H */
