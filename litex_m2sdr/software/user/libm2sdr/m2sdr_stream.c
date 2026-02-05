/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR streaming API (BladeRF-like sync)
 */

#include "m2sdr_internal.h"

#include <string.h>

#include "config.h"
#include "csr.h"

#ifdef USE_LITEPCIE
#include "litepcie_helpers.h"
#endif

static unsigned m2sdr_sample_size(enum m2sdr_format format)
{
    switch (format) {
    case M2SDR_FORMAT_SC16_Q11:
        return 4;
    default:
        return 0;
    }
}

static int m2sdr_check_buffer_size(enum m2sdr_format format, unsigned samples_per_buffer)
{
    unsigned sz = m2sdr_sample_size(format);
    if (!sz)
        return M2SDR_ERR_UNSUPPORTED;
    if (samples_per_buffer == 0)
        return M2SDR_ERR_INVAL;
    if (samples_per_buffer * sz != DMA_BUFFER_SIZE)
        return M2SDR_ERR_INVAL;
    return M2SDR_ERR_OK;
}

int m2sdr_sync_config(struct m2sdr_dev *dev,
                      enum m2sdr_module module,
                      enum m2sdr_format format,
                      unsigned num_buffers,
                      unsigned buffer_size,
                      unsigned num_transfers,
                      unsigned timeout_ms)
{
    (void)num_transfers;
    if (!dev)
        return M2SDR_ERR_INVAL;

    int rc = m2sdr_check_buffer_size(format, buffer_size);
    if (rc != M2SDR_ERR_OK)
        return rc;

#ifdef USE_LITEPCIE
    struct litepcie_dma_ctrl *dma = (module == M2SDR_RX) ? &dev->rx_dma : &dev->tx_dma;
    memset(dma, 0, sizeof(*dma));

    if (module == M2SDR_RX)
        dma->use_writer = 1;
    else
        dma->use_reader = 1;

    if (litepcie_dma_init(dma, dev->device_path, 0) < 0)
        return M2SDR_ERR_IO;

    if (module == M2SDR_RX)
        dma->writer_enable = 1;
    else
        dma->reader_enable = 1;

    if (module == M2SDR_RX) {
        dev->rx_configured = 1;
        dev->rx_format = format;
        dev->rx_buffer_size = buffer_size;
        dev->rx_timeout_ms = timeout_ms;

        /* Disable headers by default and route RX to PCIe. */
        m2sdr_writel(dev, CSR_HEADER_RX_CONTROL_ADDR,
            (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
            (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET));
        m2sdr_writel(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);
    } else {
        dev->tx_configured = 1;
        dev->tx_format = format;
        dev->tx_buffer_size = buffer_size;
        dev->tx_timeout_ms = timeout_ms;
    }

#elif defined(USE_LITEETH)
    if (!dev->udp_inited) {
        uint16_t listen_port = 2345;
        int rx_enable = 1;
        int tx_enable = 1;
        if (liteeth_udp_init(&dev->udp,
                             NULL, listen_port,
                             dev->eth_ip, dev->eth_port,
                             rx_enable, tx_enable,
                             buffer_size * m2sdr_sample_size(format),
                             num_buffers ? num_buffers : 0,
                             0) < 0) {
            return M2SDR_ERR_IO;
        }
        dev->udp_inited = 1;
    }

    if (module == M2SDR_RX) {
        dev->rx_configured = 1;
        dev->rx_format = format;
        dev->rx_buffer_size = buffer_size;
        dev->rx_timeout_ms = timeout_ms;

        /* Route RX to Ethernet. */
        m2sdr_writel(dev, CSR_HEADER_RX_CONTROL_ADDR,
            (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
            (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET));
        m2sdr_writel(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 1);
    } else {
        dev->tx_configured = 1;
        dev->tx_format = format;
        dev->tx_buffer_size = buffer_size;
        dev->tx_timeout_ms = timeout_ms;
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

#ifdef USE_LITEPCIE
static int m2sdr_wait_rx_buffer(struct m2sdr_dev *dev, char **buf, unsigned timeout_ms)
{
    int64_t start = get_time_ms();
    for (;;) {
        litepcie_dma_process(&dev->rx_dma);
        char *b = litepcie_dma_next_read_buffer(&dev->rx_dma);
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
        litepcie_dma_process(&dev->tx_dma);
        char *b = litepcie_dma_next_write_buffer(&dev->tx_dma);
        if (b) {
            *buf = b;
            return M2SDR_ERR_OK;
        }
        if (timeout_ms > 0 && (get_time_ms() - start) > (int64_t)timeout_ms)
            return M2SDR_ERR_TIMEOUT;
    }
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
        return M2SDR_ERR_UNEXPECTED;

    unsigned sample_sz = m2sdr_sample_size(dev->rx_format);
    unsigned total_bytes = num_samples * sample_sz;
    unsigned copied = 0;

    while (copied < total_bytes) {
#ifdef USE_LITEPCIE
        char *buf = NULL;
        int rc = m2sdr_wait_rx_buffer(dev, &buf, timeout_ms ? timeout_ms : dev->rx_timeout_ms);
        if (rc != M2SDR_ERR_OK)
            return rc;
        unsigned to_copy = DMA_BUFFER_SIZE;
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        memcpy((uint8_t *)samples + copied, buf, to_copy);
        copied += to_copy;
#elif defined(USE_LITEETH)
        liteeth_udp_process(&dev->udp, (timeout_ms ? timeout_ms : dev->rx_timeout_ms));
        uint8_t *buf = liteeth_udp_next_read_buffer(&dev->udp);
        if (!buf)
            return M2SDR_ERR_TIMEOUT;
        unsigned to_copy = dev->rx_buffer_size * sample_sz;
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        memcpy((uint8_t *)samples + copied, buf, to_copy);
        copied += to_copy;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
    }

    if (meta)
        memset(meta, 0, sizeof(*meta));
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
        return M2SDR_ERR_UNEXPECTED;

    unsigned sample_sz = m2sdr_sample_size(dev->tx_format);
    unsigned total_bytes = num_samples * sample_sz;
    unsigned copied = 0;

    while (copied < total_bytes) {
#ifdef USE_LITEPCIE
        char *buf = NULL;
        int rc = m2sdr_wait_tx_buffer(dev, &buf, timeout_ms ? timeout_ms : dev->tx_timeout_ms);
        if (rc != M2SDR_ERR_OK)
            return rc;
        unsigned to_copy = DMA_BUFFER_SIZE;
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        memcpy(buf, (const uint8_t *)samples + copied, to_copy);
        copied += to_copy;
#elif defined(USE_LITEETH)
        uint8_t *buf = liteeth_udp_next_write_buffer(&dev->udp);
        if (!buf)
            return M2SDR_ERR_TIMEOUT;
        unsigned to_copy = dev->tx_buffer_size * sample_sz;
        if (to_copy > total_bytes - copied)
            to_copy = total_bytes - copied;
        memcpy(buf, (const uint8_t *)samples + copied, to_copy);
        copied += to_copy;
        if (liteeth_udp_write_submit(&dev->udp) < 0)
            return M2SDR_ERR_IO;
#else
        return M2SDR_ERR_UNSUPPORTED;
#endif
    }

    if (meta)
        memset(meta, 0, sizeof(*meta));
    return M2SDR_ERR_OK;
}
