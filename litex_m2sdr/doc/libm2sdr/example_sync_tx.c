/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal TX example for libm2sdr (SC16/Q11).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "m2sdr.h"

int main(int argc, char **argv)
{
    const char *dev_id = (argc > 1) ? argv[1] : "pcie:/dev/m2sdr0";
    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    /* Keep application buffers aligned with the library's default DMA payload. */
    unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    int16_t *buf = m2sdr_alloc_buffer(format, samples_per_buf);

    if (m2sdr_open(&dev, dev_id) != 0) {
        fprintf(stderr, "m2sdr_open failed\n");
        return 1;
    }
    if (!buf) {
        fprintf(stderr, "m2sdr_alloc_buffer failed\n");
        m2sdr_close(dev);
        return 1;
    }

    /* Typical flow: start from defaults, then override only the parameters you need. */
    m2sdr_config_init(&cfg);
    cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
    cfg.clock_source   = M2SDR_CLOCK_SOURCE_INTERNAL;
    cfg.tx_freq        = 100000000; /* 100 MHz */
    cfg.tx_gain        = -5;
    if (m2sdr_apply_config(dev, &cfg) != 0) {
        fprintf(stderr, "m2sdr_apply_config failed\n");
        m2sdr_free_buffer(buf);
        m2sdr_close(dev);
        return 1;
    }

    /* Configure the blocking sync TX path before queuing samples. */
    if (m2sdr_sync_config(dev, M2SDR_TX, format,
                          0, samples_per_buf, 0, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_config failed\n");
        m2sdr_free_buffer(buf);
        m2sdr_close(dev);
        return 1;
    }

    /* Transmit one zeroed buffer as a minimal example. */
    memset(buf, 0, m2sdr_samples_to_bytes(format, samples_per_buf));
    if (m2sdr_sync_tx(dev, buf, samples_per_buf, NULL, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_tx failed\n");
        m2sdr_free_buffer(buf);
        m2sdr_close(dev);
        return 1;
    }

    m2sdr_free_buffer(buf);
    m2sdr_close(dev);
    return 0;
}
