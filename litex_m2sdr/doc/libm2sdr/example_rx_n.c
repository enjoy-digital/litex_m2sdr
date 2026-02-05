/* SPDX-License-Identifier: BSD-2-Clause
 *
 * RX N buffers example with metadata.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "m2sdr.h"
#include "config.h" /* DMA_BUFFER_SIZE */

int main(int argc, char **argv)
{
    const char *dev_id = (argc > 1) ? argv[1] : "pcie:/dev/m2sdr0";
    int nbuf = (argc > 2) ? atoi(argv[2]) : 8;

    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;

    if (m2sdr_open(&dev, dev_id) != 0) {
        fprintf(stderr, "m2sdr_open failed\n");
        return 1;
    }

    m2sdr_config_init(&cfg);
    cfg.rx_freq = 100000000; /* 100 MHz */
    cfg.rx_gain1 = 10;
    cfg.rx_gain2 = 10;
    if (m2sdr_apply_config(dev, &cfg) != 0) {
        fprintf(stderr, "m2sdr_apply_config failed\n");
        m2sdr_close(dev);
        return 1;
    }

    unsigned samples_per_buf = DMA_BUFFER_SIZE / 4; /* SC16 */
    if (m2sdr_sync_config(dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11,
                          0, samples_per_buf, 0, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_config failed\n");
        m2sdr_close(dev);
        return 1;
    }

    int16_t buf[DMA_BUFFER_SIZE / 2];
    for (int i = 0; i < nbuf; i++) {
        struct m2sdr_metadata meta;
        if (m2sdr_sync_rx(dev, buf, samples_per_buf, &meta, 1000) != 0) {
            fprintf(stderr, "m2sdr_sync_rx failed\n");
            m2sdr_close(dev);
            return 1;
        }
        if (meta.flags & M2SDR_META_FLAG_HAS_TIME) {
            printf("buf %d ts=%" PRIu64 "\n", i, meta.timestamp);
        } else {
            printf("buf %d\n", i);
        }
    }

    m2sdr_close(dev);
    return 0;
}
