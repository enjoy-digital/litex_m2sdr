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

#include <string.h>

#include "config.h"
#include "csr.h"
#include "m2sdr_internal.h"

#ifdef USE_LITEPCIE
#include "litepcie_helpers.h"
#endif

#ifdef USE_LITEETH
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define M2SDR_DMA_HEADER_SIZE 16
#define M2SDR_DMA_HEADER_SYNC_WORD 0x5aa55aa55aa55aa5ULL
#define M2SDR_LITEETH_DEFAULT_SOCKET_BUFFER_BYTES (8 * 1024 * 1024)
#define M2SDR_LITEETH_RX_RECOVERY_TIMEOUT_MS 50
#define M2SDR_LITEETH_TX_DRAIN_DELAY_US 1000

/* Helpers */
/*---------*/

static unsigned m2sdr_sample_size(enum m2sdr_format format)
{
    switch (format) {
    case M2SDR_FORMAT_SC16_Q11:
        return 4;
    case M2SDR_FORMAT_SC8_Q7:
        return 2;
    default:
        return 0;
    }
}

static int m2sdr_parse_dma_header(const uint8_t *buf, uint64_t *timestamp)
{
    uint64_t sync_word = 0;
    uint64_t ts = 0;

    memcpy(&sync_word, buf, sizeof(sync_word));
    if (sync_word != M2SDR_DMA_HEADER_SYNC_WORD)
        return 0;

    memcpy(&ts, buf + 8, sizeof(ts));
    *timestamp = ts;
    return 1;
}

static void m2sdr_write_dma_header(uint8_t *buf, uint64_t timestamp)
{
    const uint64_t sync_word = M2SDR_DMA_HEADER_SYNC_WORD;

    memcpy(buf, &sync_word, sizeof(sync_word));
    memcpy(buf + 8, &timestamp, sizeof(timestamp));
}

static int m2sdr_check_buffer_size(enum m2sdr_format format, unsigned samples_per_buffer, unsigned bytes_per_buffer)
{
    unsigned sz = m2sdr_sample_size(format);
    if (!sz)
        return M2SDR_ERR_UNSUPPORTED;
    if (samples_per_buffer == 0)
        return M2SDR_ERR_INVAL;
    if (samples_per_buffer * sz != bytes_per_buffer)
        return M2SDR_ERR_INVAL;
    return M2SDR_ERR_OK;
}

static unsigned m2sdr_stream_payload_bytes(struct m2sdr_dev *dev,
                                           enum m2sdr_direction direction,
                                           enum m2sdr_format format)
{
    unsigned bytes_per_buffer = DMA_BUFFER_SIZE;

    (void)format;

    /* The public sync API exposes payload samples. Header bytes are accounted
     * for here so the caller does not need backend-specific math. */
    if (direction == M2SDR_RX && dev->rx_header_enable && dev->rx_strip_header)
        bytes_per_buffer = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
    if (direction == M2SDR_TX && dev->tx_header_enable)
        bytes_per_buffer = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;

    return bytes_per_buffer;
}

static void m2sdr_store_stream_config(struct m2sdr_dev *dev,
                                      enum m2sdr_direction direction,
                                      enum m2sdr_format format,
                                      unsigned buffer_size,
                                      unsigned timeout_ms)
{
    /* Keep the active stream description in the device object so the blocking
     * sync helpers and zero-copy helpers use one shared source of truth. */
    if (direction == M2SDR_RX) {
        dev->rx_configured  = 1;
        dev->rx_format      = format;
        dev->rx_buffer_size = buffer_size;
        dev->rx_timeout_ms  = timeout_ms;
    } else {
        dev->tx_configured  = 1;
        dev->tx_format      = format;
        dev->tx_buffer_size = buffer_size;
        dev->tx_timeout_ms  = timeout_ms;
    }
}

#ifdef USE_LITEETH
static int m2sdr_liteeth_get_local_ip_for_route(const char *remote_ip, uint16_t remote_port, uint32_t *local_ip)
{
    struct sockaddr_in remote_addr;
    struct sockaddr_in local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    int sock = -1;
    int ret = M2SDR_ERR_IO;

    if (!remote_ip || !local_ip)
        return M2SDR_ERR_INVAL;

    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_ip, &remote_addr.sin_addr) != 1)
        return M2SDR_ERR_INVAL;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return M2SDR_ERR_IO;

    /* UDP connect performs the kernel route lookup without sending traffic. */
    if (connect(sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0)
        goto out;

    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(sock, (struct sockaddr *)&local_addr, &local_addr_len) < 0)
        goto out;
    if (local_addr.sin_family != AF_INET)
        goto out;

    *local_ip = ntohl(local_addr.sin_addr.s_addr);
    ret = M2SDR_ERR_OK;

out:
    close(sock);
    return ret;
}

void m2sdr_liteeth_rx_stream_config_init(struct m2sdr_liteeth_rx_stream_config *config)
{
    if (!config)
        return;

    memset(config, 0, sizeof(*config));
    config->mode = M2SDR_LITEETH_RX_MODE_UDP;
    config->udp_port = 2345;
}

int m2sdr_liteeth_get_local_ip(struct m2sdr_dev *dev, uint32_t *local_ip)
{
    if (!dev || !local_ip)
        return M2SDR_ERR_INVAL;

    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;

    return m2sdr_liteeth_get_local_ip_for_route(dev->eth_ip, dev->eth_port, local_ip);
}

int m2sdr_liteeth_rx_stream_prepare(struct m2sdr_dev *dev,
                                    const struct m2sdr_liteeth_rx_stream_config *config)
{
    uint32_t local_ip = 0;
    int rc;

    if (!dev || !config)
        return M2SDR_ERR_INVAL;
    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;

    local_ip = config->local_ip;
    if (local_ip == 0) {
        rc = m2sdr_liteeth_get_local_ip(dev, &local_ip);
        if (rc != M2SDR_ERR_OK)
            return rc;
    }

    switch (config->mode) {
    case M2SDR_LITEETH_RX_MODE_UDP:
#if defined(CSR_ETH_RX_STREAMER_IP_ADDRESS_ADDR) && \
    defined(CSR_ETH_RX_STREAMER_UDP_PORT_ADDR)
    if (m2sdr_reg_write(dev, CSR_ETH_RX_STREAMER_IP_ADDRESS_ADDR, local_ip) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_ETH_RX_STREAMER_UDP_PORT_ADDR, config->udp_port) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif

    case M2SDR_LITEETH_RX_MODE_VRT:
#if defined(CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR) && \
    defined(CSR_VRT_STREAMER_VRT_STREAMER_IP_ADDRESS_ADDR) && \
    defined(CSR_VRT_STREAMER_VRT_STREAMER_UDP_PORT_ADDR)
    if (m2sdr_reg_write(dev, CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR, 0) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_VRT_STREAMER_VRT_STREAMER_IP_ADDRESS_ADDR, local_ip) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_VRT_STREAMER_VRT_STREAMER_UDP_PORT_ADDR, config->udp_port) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif

    case M2SDR_LITEETH_RX_MODE_DISABLED:
        return M2SDR_ERR_OK;

    default:
        return M2SDR_ERR_INVAL;
    }
}

int m2sdr_liteeth_rx_stream_activate(struct m2sdr_dev *dev,
                                     const struct m2sdr_liteeth_rx_stream_config *config)
{
    int rc;

    if (!dev || !config)
        return M2SDR_ERR_INVAL;

    rc = m2sdr_liteeth_rx_stream_prepare(dev, config);
    if (rc != M2SDR_ERR_OK)
        return rc;

    switch (config->mode) {
    case M2SDR_LITEETH_RX_MODE_UDP:
#ifdef CSR_ETH_RX_STREAMER_ENABLE_ADDR
        if (m2sdr_reg_write(dev, CSR_ETH_RX_STREAMER_ENABLE_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
#ifdef CSR_ETH_RX_MODE_ADDR
        if (m2sdr_reg_write(dev, CSR_ETH_RX_MODE_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
#endif
        break;

    case M2SDR_LITEETH_RX_MODE_VRT:
#ifdef CSR_ETH_RX_MODE_ADDR
        if (m2sdr_reg_write(dev, CSR_ETH_RX_MODE_ADDR, 2) != 0)
            return M2SDR_ERR_IO;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
#ifdef CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR
        if (m2sdr_reg_write(dev, CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
        break;

    case M2SDR_LITEETH_RX_MODE_DISABLED:
        return m2sdr_liteeth_rx_stream_deactivate(dev);

    default:
        return M2SDR_ERR_INVAL;
    }

    if (m2sdr_reg_write(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 1) != 0)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
}

int m2sdr_liteeth_rx_stream_deactivate(struct m2sdr_dev *dev)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;

#ifdef CSR_ETH_RX_MODE_ADDR
    /* Raw streamer enable is not a safe stop on all bitstreams. Use the
     * Ethernet RX mode flush branch when present, then route RX away. */
    (void)m2sdr_reg_write(dev, CSR_ETH_RX_MODE_ADDR, 0);
#endif
#ifdef CSR_ETH_RX_STREAMER_ENABLE_ADDR
    (void)m2sdr_reg_write(dev, CSR_ETH_RX_STREAMER_ENABLE_ADDR, 0);
#endif
#ifdef CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR
    (void)m2sdr_reg_write(dev, CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR, 0);
#endif
    (void)m2sdr_reg_write(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);
    return M2SDR_ERR_OK;
}

int m2sdr_liteeth_tx_stream_activate(struct m2sdr_dev *dev)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;

#ifdef CSR_ETH_TX_STREAMER_ENABLE_ADDR
    if (m2sdr_reg_write(dev, CSR_ETH_TX_STREAMER_ENABLE_ADDR, 1) != 0)
        return M2SDR_ERR_IO;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
#ifdef CSR_CROSSBAR_MUX_SEL_ADDR
    if (m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 1) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_liteeth_tx_stream_deactivate(struct m2sdr_dev *dev)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;

#ifdef CSR_CROSSBAR_MUX_SEL_ADDR
    (void)m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0);
#endif
#ifdef CSR_ETH_TX_STREAMER_ENABLE_ADDR
    (void)m2sdr_reg_write(dev, CSR_ETH_TX_STREAMER_ENABLE_ADDR, 0);
#endif
    /* Gateware drains the TX streamer FIFO while disabled; this delay covers a
     * full 256 KiB FIFO at the Ethernet-only 100 MHz sys_clk with margin. */
    usleep(M2SDR_LITEETH_TX_DRAIN_DELAY_US);
    return M2SDR_ERR_OK;
}

int m2sdr_liteeth_set_rx_timeout_recovery(struct m2sdr_dev *dev, bool enable)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;

    dev->liteeth_rx_timeout_recovery_disabled = enable ? 0 : 1;
    if (!enable)
        dev->liteeth_rx_timeout_recovery_armed = 0;
    else if (dev->liteeth_rx_config_valid)
        dev->liteeth_rx_timeout_recovery_armed = 1;

    return M2SDR_ERR_OK;
}

int m2sdr_liteeth_get_udp_stats(struct m2sdr_dev *dev,
                                struct m2sdr_liteeth_udp_stats *stats)
{
    if (!dev || !stats)
        return M2SDR_ERR_INVAL;
    if (dev->transport != M2SDR_TRANSPORT_LITEETH)
        return M2SDR_ERR_UNSUPPORTED;
    if (!dev->udp_inited)
        return M2SDR_ERR_STATE;

    memset(stats, 0, sizeof(*stats));
    stats->buffer_size           = dev->udp.buf_size;
    stats->buffer_count          = dev->udp.buf_count;
    stats->rx_buffers            = dev->udp.rx_buffers;
    stats->rx_ring_full_events   = dev->udp.rx_ring_full_events;
    stats->rx_flushes            = dev->udp.rx_flushes;
    stats->rx_flush_bytes        = dev->udp.rx_flush_bytes;
    stats->rx_kernel_drops       = dev->udp.rx_kernel_drops;
    stats->rx_source_drops       = dev->udp.rx_source_drops;
    stats->rx_timeout_recoveries = dev->udp.rx_timeout_recoveries;
    stats->rx_recv_errors        = dev->udp.rx_recv_errors;
    stats->tx_buffers            = dev->udp.tx_buffers;
    stats->tx_bytes              = dev->udp.tx_bytes;
    stats->tx_send_errors        = dev->udp.tx_send_errors;
    stats->so_rcvbuf_requested   = dev->udp.so_rcvbuf_bytes;
    stats->so_rcvbuf_actual      = dev->udp.so_rcvbuf_actual_bytes;
    stats->so_sndbuf_requested   = dev->udp.so_sndbuf_bytes;
    stats->so_sndbuf_actual      = dev->udp.so_sndbuf_actual_bytes;
    return M2SDR_ERR_OK;
}
#else
void m2sdr_liteeth_rx_stream_config_init(struct m2sdr_liteeth_rx_stream_config *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->mode = M2SDR_LITEETH_RX_MODE_UDP;
    config->udp_port = 2345;
}

int m2sdr_liteeth_get_local_ip(struct m2sdr_dev *dev, uint32_t *local_ip)
{
    (void)dev;
    (void)local_ip;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_rx_stream_prepare(struct m2sdr_dev *dev,
                                    const struct m2sdr_liteeth_rx_stream_config *config)
{
    (void)dev;
    (void)config;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_rx_stream_activate(struct m2sdr_dev *dev,
                                     const struct m2sdr_liteeth_rx_stream_config *config)
{
    (void)dev;
    (void)config;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_rx_stream_deactivate(struct m2sdr_dev *dev)
{
    (void)dev;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_tx_stream_activate(struct m2sdr_dev *dev)
{
    (void)dev;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_tx_stream_deactivate(struct m2sdr_dev *dev)
{
    (void)dev;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_set_rx_timeout_recovery(struct m2sdr_dev *dev, bool enable)
{
    (void)dev;
    (void)enable;
    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_liteeth_get_udp_stats(struct m2sdr_dev *dev,
                                struct m2sdr_liteeth_udp_stats *stats)
{
    (void)dev;
    (void)stats;
    return M2SDR_ERR_UNSUPPORTED;
}
#endif

/* Public API */
/*------------*/

int m2sdr_sync_config(struct m2sdr_dev *dev,
                      enum m2sdr_direction direction,
                      enum m2sdr_format format,
                      unsigned num_buffers,
                      unsigned buffer_size,
                      unsigned num_transfers,
                      unsigned timeout_ms)
{
    (void)num_transfers;
    if (!dev)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX && direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;

    /* The current backends stream fixed-size DMA payloads. The public API keeps
     * buffer sizes in samples to stay format-centric. */
    unsigned bytes_per_buffer = m2sdr_stream_payload_bytes(dev, direction, format);

    int rc = m2sdr_check_buffer_size(format, buffer_size, bytes_per_buffer);
    if (rc != M2SDR_ERR_OK)
        return rc;

#ifdef USE_LITEPCIE
    struct litepcie_dma_ctrl *dma = (direction == M2SDR_RX) ? &dev->rx_dma : &dev->tx_dma;
    memset(dma, 0, sizeof(*dma));

    /* libm2sdr keeps the LitePCIe DMA setup internal and exposes only the
     * blocking sample-centric sync API on top. */
    if (direction == M2SDR_RX)
        dma->use_writer = 1;
    else
        dma->use_reader = 1;

    if (litepcie_dma_init(dma, dev->device_path, dev->zero_copy ? 1 : 0) < 0)
        return M2SDR_ERR_IO;

    if (direction == M2SDR_RX)
        dma->writer_enable = 1;
    else
        dma->reader_enable = 1;

    if (direction == M2SDR_RX) {
        m2sdr_store_stream_config(dev, direction, format, buffer_size, timeout_ms);

        (void)m2sdr_liteeth_rx_stream_deactivate(dev);

        if (!dev->rx_header_enable) {
            if (m2sdr_reg_write(dev, CSR_HEADER_RX_CONTROL_ADDR,
                (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
                (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)) != 0)
                return M2SDR_ERR_IO;
        }
        if (m2sdr_reg_write(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0) != 0)
            return M2SDR_ERR_IO;
    } else {
        m2sdr_store_stream_config(dev, direction, format, buffer_size, timeout_ms);
    }

#elif defined(USE_LITEETH)
    const uint16_t listen_port = 2345;
    struct m2sdr_liteeth_rx_stream_config eth_rx_config;
    int eth_rx_needs_activate = 0;

    if (direction == M2SDR_RX) {
        m2sdr_store_stream_config(dev, direction, format, buffer_size, timeout_ms);

        if (!dev->rx_header_enable) {
            if (m2sdr_reg_write(dev, CSR_HEADER_RX_CONTROL_ADDR,
                (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
                (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)) != 0)
                return M2SDR_ERR_IO;
        }

        m2sdr_liteeth_rx_stream_config_init(&eth_rx_config);
        eth_rx_config.mode = M2SDR_LITEETH_RX_MODE_UDP;
        eth_rx_config.udp_port = listen_port;
        rc = m2sdr_liteeth_rx_stream_prepare(dev, &eth_rx_config);
        if (rc != M2SDR_ERR_OK)
            return rc;
        eth_rx_needs_activate = 1;
    }

    if (!dev->udp_inited) {
        /* The UDP helper owns the packet ring. libm2sdr maps one UDP payload
         * to one public sync buffer. */
        if (liteeth_udp_init(&dev->udp,
                             NULL, listen_port,
                             dev->eth_ip, listen_port,
                             1, 1,
                             buffer_size * m2sdr_sample_size(format),
                             num_buffers ? num_buffers : 0,
                             0) < 0) {
            return M2SDR_ERR_IO;
        }
        (void)liteeth_udp_set_so_rcvbuf(&dev->udp, M2SDR_LITEETH_DEFAULT_SOCKET_BUFFER_BYTES);
        (void)liteeth_udp_set_so_sndbuf(&dev->udp, M2SDR_LITEETH_DEFAULT_SOCKET_BUFFER_BYTES);
        (void)liteeth_udp_set_rx_source_filter(&dev->udp, dev->eth_ip, 0);
        dev->udp_inited = 1;
    }

    if (eth_rx_needs_activate) {
        liteeth_udp_flush_rx(&dev->udp);
        rc = m2sdr_liteeth_rx_stream_activate(dev, &eth_rx_config);
        if (rc != M2SDR_ERR_OK)
            return rc;
        dev->liteeth_rx_config = eth_rx_config;
        dev->liteeth_rx_config_valid = 1;
        dev->liteeth_rx_timeout_recovery_armed = 1;
    }

    if (direction == M2SDR_TX) {
        (void)m2sdr_liteeth_tx_stream_deactivate(dev);

        rc = m2sdr_liteeth_tx_stream_activate(dev);
        if (rc != M2SDR_ERR_OK)
            return rc;
        m2sdr_store_stream_config(dev, direction, format, buffer_size, timeout_ms);
    }
#else
    (void)num_buffers;
    (void)buffer_size;
    (void)format;
    (void)timeout_ms;
    return M2SDR_ERR_UNSUPPORTED;
#endif

    return M2SDR_ERR_OK;
}

void m2sdr_sync_params_init(struct m2sdr_sync_params *params)
{
    if (!params)
        return;
    memset(params, 0, sizeof(*params));
    params->timeout_ms = 1000;
    params->format = M2SDR_FORMAT_SC16_Q11;
}

void m2sdr_stream_config_init(m2sdr_stream_config_t *config)
{
    m2sdr_sync_params_init(config);
}

int m2sdr_sync_config_ex(struct m2sdr_dev *dev, const struct m2sdr_sync_params *params)
{
    if (!dev || !params)
        return M2SDR_ERR_INVAL;
    if (params->direction != M2SDR_RX && params->direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;

    dev->zero_copy = params->zero_copy ? 1 : 0;
    if (params->direction == M2SDR_RX) {
        int rc = m2sdr_set_rx_header(dev, params->rx_header_enable, params->rx_strip_header);
        if (rc != M2SDR_ERR_OK)
            return rc;
    } else {
        int rc = m2sdr_set_tx_header(dev, params->tx_header_enable);
        if (rc != M2SDR_ERR_OK)
            return rc;
    }

    return m2sdr_sync_config(dev,
                             params->direction,
                             params->format,
                             params->num_buffers,
                             params->buffer_size,
                             params->num_transfers,
                             params->timeout_ms);
}

int m2sdr_stream_configure(struct m2sdr_dev *dev, const m2sdr_stream_config_t *config)
{
    return m2sdr_sync_config_ex(dev, config);
}

void m2sdr_stream_cleanup(struct m2sdr_dev *dev)
{
    if (!dev)
        return;

#ifdef USE_LITEPCIE
    if (dev->rx_configured)
        litepcie_dma_cleanup(&dev->rx_dma);
    if (dev->tx_configured)
        litepcie_dma_cleanup(&dev->tx_dma);
#elif defined(USE_LITEETH)
    if (dev->rx_configured) {
        (void)m2sdr_liteeth_rx_stream_deactivate(dev);
    }
    if (dev->tx_configured) {
        (void)m2sdr_liteeth_tx_stream_deactivate(dev);
    }
    dev->liteeth_rx_config_valid = 0;
    dev->liteeth_rx_timeout_recovery_armed = 0;
#endif

    dev->rx_configured = 0;
    dev->tx_configured = 0;
}

#ifdef USE_LITEPCIE
/* PCIe sync helpers wait for the next DMA ring entry and enforce the public
 * timeout semantics in milliseconds. */
static int m2sdr_wait_rx_buffer(struct m2sdr_dev *dev, char **buf, unsigned timeout_ms)
{
    int64_t start = get_time_ms();
    for (;;) {
        /* Drain any buffers already staged in userspace before polling the DMA
         * helper again. Re-running litepcie_dma_process() too early refreshes
         * the batch counters and drops the unread remainder. */
        char *b = litepcie_dma_next_read_buffer(&dev->rx_dma);
        if (b) {
            *buf = b;
            return M2SDR_ERR_OK;
        }
        litepcie_dma_process(&dev->rx_dma);
        b = litepcie_dma_next_read_buffer(&dev->rx_dma);
        if (b) {
            *buf = b;
            return M2SDR_ERR_OK;
        }
        if (timeout_ms > 0 && (get_time_ms() - start) > (int64_t)timeout_ms)
            return M2SDR_ERR_TIMEOUT;
    }
}

static int m2sdr_wait_tx_buffer(struct m2sdr_dev *dev, char **buf, unsigned timeout_ms)
{
    int64_t start = get_time_ms();
    for (;;) {
        /* Mirror the RX-side batching behavior for TX so partially-consumed
         * userspace batches are not overwritten by a fresh DMA poll. */
        char *b = litepcie_dma_next_write_buffer(&dev->tx_dma);
        if (b) {
            *buf = b;
            return M2SDR_ERR_OK;
        }
        litepcie_dma_process(&dev->tx_dma);
        b = litepcie_dma_next_write_buffer(&dev->tx_dma);
        if (b) {
            *buf = b;
            return M2SDR_ERR_OK;
        }
        if (timeout_ms > 0 && (get_time_ms() - start) > (int64_t)timeout_ms)
            return M2SDR_ERR_TIMEOUT;
    }
}
#endif

#ifdef USE_LITEETH
static int m2sdr_liteeth_wait_rx_buffer(struct m2sdr_dev *dev,
                                        unsigned timeout_ms,
                                        uint8_t **buf)
{
    if (!dev || !buf)
        return M2SDR_ERR_INVAL;

    liteeth_udp_process(&dev->udp, timeout_ms);
    *buf = liteeth_udp_next_read_buffer(&dev->udp);
    if (*buf) {
        dev->liteeth_rx_timeout_recovery_armed = 1;
        return M2SDR_ERR_OK;
    }

    if (!dev->liteeth_rx_config_valid ||
        dev->liteeth_rx_timeout_recovery_disabled ||
        !dev->liteeth_rx_timeout_recovery_armed ||
        timeout_ms == 0) {
        return M2SDR_ERR_TIMEOUT;
    }

    dev->liteeth_rx_timeout_recovery_armed = 0;
    dev->udp.rx_timeout_recoveries++;

    (void)m2sdr_liteeth_rx_stream_deactivate(dev);
    liteeth_udp_flush_rx(&dev->udp);

    int rc = m2sdr_liteeth_rx_stream_activate(dev, &dev->liteeth_rx_config);
    if (rc != M2SDR_ERR_OK)
        return rc;

    unsigned retry_ms = timeout_ms;
    if (retry_ms > M2SDR_LITEETH_RX_RECOVERY_TIMEOUT_MS)
        retry_ms = M2SDR_LITEETH_RX_RECOVERY_TIMEOUT_MS;

    liteeth_udp_process(&dev->udp, retry_ms);
    *buf = liteeth_udp_next_read_buffer(&dev->udp);
    if (*buf) {
        dev->liteeth_rx_timeout_recovery_armed = 1;
        return M2SDR_ERR_OK;
    }

    return M2SDR_ERR_TIMEOUT;
}
#endif

int m2sdr_sync_rx(struct m2sdr_dev *dev,
                  void *samples,
                  unsigned num_samples,
                  struct m2sdr_metadata *meta,
                  unsigned timeout_ms)
{
    if (!dev || !samples)
        return M2SDR_ERR_INVAL;
    if (!dev->rx_configured)
        return M2SDR_ERR_STATE;
    /* Clear metadata once up-front so timestamps survive a successful read. */
    if (meta)
        memset(meta, 0, sizeof(*meta));

    unsigned sample_sz = m2sdr_sample_size(dev->rx_format);
    unsigned total_bytes = num_samples * sample_sz;
    unsigned copied = 0;

    while (copied < total_bytes) {
#ifdef USE_LITEPCIE
        char *buf = NULL;
        int rc = m2sdr_wait_rx_buffer(dev, &buf, timeout_ms ? timeout_ms : dev->rx_timeout_ms);
        if (rc != M2SDR_ERR_OK)
            return rc;
        /* RX metadata is currently carried only by the optional 16-byte FPGA
         * header, so parse it before copying the payload out. */
        unsigned to_copy = DMA_BUFFER_SIZE;
        unsigned payload_off = 0;
        if (dev->rx_header_enable && dev->rx_strip_header) {
            payload_off = M2SDR_DMA_HEADER_SIZE;
            to_copy = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
        }
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        if (dev->rx_header_enable) {
            uint64_t ts = 0;
            if (m2sdr_parse_dma_header((const uint8_t *)buf, &ts) && meta) {
                meta->timestamp = ts;
                meta->flags |= M2SDR_META_FLAG_HAS_TIME;
            }
        }
        memcpy((uint8_t *)samples + copied, buf + payload_off, to_copy);
        copied += to_copy;
#elif defined(USE_LITEETH)
        uint8_t *buf = NULL;
        int rc = m2sdr_liteeth_wait_rx_buffer(dev,
                                              (timeout_ms ? timeout_ms : dev->rx_timeout_ms),
                                              &buf);
        if (rc != M2SDR_ERR_OK)
            return rc;
        unsigned to_copy = dev->rx_buffer_size * sample_sz;
        unsigned payload_off = 0;
        if (dev->rx_header_enable && dev->rx_strip_header) {
            payload_off = M2SDR_DMA_HEADER_SIZE;
            to_copy = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
        }
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        if (dev->rx_header_enable) {
            uint64_t ts = 0;
            if (m2sdr_parse_dma_header(buf, &ts) && meta) {
                meta->timestamp = ts;
                meta->flags |= M2SDR_META_FLAG_HAS_TIME;
            }
        }
        memcpy((uint8_t *)samples + copied, buf + payload_off, to_copy);
        copied += to_copy;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
    }

    return M2SDR_ERR_OK;
}

int m2sdr_sync_tx(struct m2sdr_dev *dev,
                  const void *samples,
                  unsigned num_samples,
                  struct m2sdr_metadata *meta,
                  unsigned timeout_ms)
{
    if (!dev || !samples)
        return M2SDR_ERR_INVAL;
    if (!dev->tx_configured)
        return M2SDR_ERR_STATE;

    unsigned sample_sz = m2sdr_sample_size(dev->tx_format);
    unsigned total_bytes = num_samples * sample_sz;
    unsigned copied = 0;

    while (copied < total_bytes) {
#ifdef USE_LITEPCIE
        char *buf = NULL;
        int rc = m2sdr_wait_tx_buffer(dev, &buf, timeout_ms ? timeout_ms : dev->tx_timeout_ms);
        if (rc != M2SDR_ERR_OK)
            return rc;
        /* When enabled, the header is synthesized by libm2sdr from the public
         * metadata structure before the payload is copied in. */
        unsigned to_copy = DMA_BUFFER_SIZE;
        unsigned payload_off = 0;
        if (dev->tx_header_enable) {
            payload_off = M2SDR_DMA_HEADER_SIZE;
            to_copy = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
            uint64_t ts = 0;
            if (meta && (meta->flags & M2SDR_META_FLAG_HAS_TIME))
                ts = meta->timestamp;
            m2sdr_write_dma_header((uint8_t *)buf, ts);
        }
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        memcpy(buf + payload_off, (const uint8_t *)samples + copied, to_copy);
        copied += to_copy;
#elif defined(USE_LITEETH)
        uint8_t *buf = liteeth_udp_next_write_buffer(&dev->udp);
        if (!buf)
            return M2SDR_ERR_TIMEOUT;
        unsigned to_copy = dev->tx_buffer_size * sample_sz;
        unsigned payload_off = 0;
        if (dev->tx_header_enable) {
            payload_off = M2SDR_DMA_HEADER_SIZE;
            to_copy = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
            uint64_t ts = 0;
            if (meta && (meta->flags & M2SDR_META_FLAG_HAS_TIME))
                ts = meta->timestamp;
            m2sdr_write_dma_header(buf, ts);
        }
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        memcpy(buf + payload_off, (const uint8_t *)samples + copied, to_copy);
        copied += to_copy;
        if (liteeth_udp_write_submit(&dev->udp) < 0)
            return M2SDR_ERR_IO;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
    }

    return M2SDR_ERR_OK;
}

int m2sdr_get_buffer(struct m2sdr_dev *dev,
                     enum m2sdr_direction direction,
                     void **buffer,
                     unsigned *num_samples,
                     unsigned timeout_ms)
{
    if (!dev || !buffer || !num_samples)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX && direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;

    unsigned sample_sz = 0;
    unsigned bytes_per_buffer = DMA_BUFFER_SIZE;
    unsigned payload_off = 0;

    if (direction == M2SDR_RX) {
        if (!dev->rx_configured)
            return M2SDR_ERR_STATE;
        sample_sz = m2sdr_sample_size(dev->rx_format);
        if (dev->rx_header_enable && dev->rx_strip_header) {
            payload_off = M2SDR_DMA_HEADER_SIZE;
            bytes_per_buffer = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
        }
    } else {
        if (!dev->tx_configured)
            return M2SDR_ERR_STATE;
        sample_sz = m2sdr_sample_size(dev->tx_format);
        if (dev->tx_header_enable) {
            payload_off = M2SDR_DMA_HEADER_SIZE;
            bytes_per_buffer = DMA_BUFFER_SIZE - M2SDR_DMA_HEADER_SIZE;
        }
    }

    if (!sample_sz)
        return M2SDR_ERR_UNSUPPORTED;

#ifdef USE_LITEPCIE
    if (direction == M2SDR_RX) {
        char *buf = NULL;
        int rc = m2sdr_wait_rx_buffer(dev, &buf, timeout_ms ? timeout_ms : dev->rx_timeout_ms);
        if (rc != M2SDR_ERR_OK)
            return rc;
        *buffer = buf + payload_off;
    } else {
        char *buf = NULL;
        int rc = m2sdr_wait_tx_buffer(dev, &buf, timeout_ms ? timeout_ms : dev->tx_timeout_ms);
        if (rc != M2SDR_ERR_OK)
            return rc;
        *buffer = buf + payload_off;
    }
#elif defined(USE_LITEETH)
    if (direction == M2SDR_RX) {
        uint8_t *buf = NULL;
        int rc = m2sdr_liteeth_wait_rx_buffer(dev,
                                              (timeout_ms ? timeout_ms : dev->rx_timeout_ms),
                                              &buf);
        if (rc != M2SDR_ERR_OK)
            return rc;
        *buffer = buf + payload_off;
    } else {
        uint8_t *buf = liteeth_udp_next_write_buffer(&dev->udp);
        if (!buf)
            return M2SDR_ERR_TIMEOUT;
        *buffer = buf + payload_off;
    }
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif

    *num_samples = bytes_per_buffer / sample_sz;
    return M2SDR_ERR_OK;
}

int m2sdr_submit_buffer(struct m2sdr_dev *dev,
                        enum m2sdr_direction direction,
                        void *buffer,
                        unsigned num_samples,
                        struct m2sdr_metadata *meta)
{
    if (!dev || !buffer)
        return M2SDR_ERR_INVAL;

    if (direction != M2SDR_TX)
        return M2SDR_ERR_INVAL;

    unsigned sample_sz = m2sdr_sample_size(dev->tx_format);
    if (!sample_sz)
        return M2SDR_ERR_UNSUPPORTED;

    if (dev->tx_header_enable) {
        uint8_t *base = (uint8_t *)buffer - M2SDR_DMA_HEADER_SIZE;
        uint64_t ts = 0;
        if (meta && (meta->flags & M2SDR_META_FLAG_HAS_TIME))
            ts = meta->timestamp;
        m2sdr_write_dma_header(base, ts);
    }

    (void)num_samples;

#ifdef USE_LITEPCIE
    /* No explicit submit step required for zero-copy DMA buffers. */
    return M2SDR_ERR_OK;
#elif defined(USE_LITEETH)
    if (liteeth_udp_write_submit(&dev->udp) < 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_release_buffer(struct m2sdr_dev *dev,
                         enum m2sdr_direction direction,
                         void *buffer)
{
    if (!dev || !buffer)
        return M2SDR_ERR_INVAL;
    if (direction != M2SDR_RX)
        return M2SDR_ERR_INVAL;
    /* DMA/UDP ring advances on read; no explicit release step is currently
     * required, but keep this function for API symmetry. */
    return M2SDR_ERR_OK;
}
