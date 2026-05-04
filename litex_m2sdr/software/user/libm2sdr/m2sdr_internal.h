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
    struct m2sdr_liteeth_rx_stream_config liteeth_rx_config;
    int liteeth_rx_config_valid;
    int liteeth_rx_timeout_recovery_armed;
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
    struct ad9361_rf_phy *ad9361_phy;
};

int m2sdr_hal_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
int m2sdr_hal_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);
void m2sdr_stream_cleanup(struct m2sdr_dev *dev);

int m2sdr_test_parse_identifier(const char *id, uint16_t *port_out);

#endif /* M2SDR_INTERNAL_H */
