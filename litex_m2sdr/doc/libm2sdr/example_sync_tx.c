/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal TX example for libm2sdr (SC16/Q11).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "m2sdr.h"
#include "config.h" /* DMA_BUFFER_SIZE */

int main(int argc, char **argv)
{
    const char *dev_id = (argc > 1) ? argv[1] : "pcie:/dev/m2sdr0";
    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;

    if (m2sdr_open(&dev, dev_id) != 0) {
        fprintf(stderr, "m2sdr_open failed\n");
        return 1;
    }

    m2sdr_config_init(&cfg);
    cfg.tx_freq = 100000000; /* 100 MHz */
    cfg.tx_gain = -5;
    if (m2sdr_apply_config(dev, &cfg) != 0) {
        fprintf(stderr, "m2sdr_apply_config failed\n");
        m2sdr_close(dev);
        return 1;
    }

    unsigned samples_per_buf = DMA_BUFFER_SIZE / 4; /* SC16 */
    if (m2sdr_sync_config(dev, M2SDR_TX, M2SDR_FORMAT_SC16_Q11,
                          0, samples_per_buf, 0, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_config failed\n");
        m2sdr_close(dev);
        return 1;
    }

    int16_t buf[DMA_BUFFER_SIZE / 2];
    memset(buf, 0, sizeof(buf));
    if (m2sdr_sync_tx(dev, buf, samples_per_buf, NULL, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_tx failed\n");
        m2sdr_close(dev);
        return 1;
    }

    m2sdr_close(dev);
    return 0;
}
