/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tone TX example for libm2sdr (SC16/SC8).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "m2sdr.h"
#include "config.h" /* DMA_BUFFER_SIZE */

int main(int argc, char **argv)
{
    const char *dev_id = (argc > 1) ? argv[1] : "pcie:/dev/m2sdr0";
    bool use_sc8 = (argc > 2) && (strcmp(argv[2], "sc8") == 0);
    enum m2sdr_format format = use_sc8 ? M2SDR_FORMAT_SC8_Q7 : M2SDR_FORMAT_SC16_Q11;

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

    unsigned sample_size = m2sdr_format_size(format);
    unsigned samples_per_buf = DMA_BUFFER_SIZE / sample_size;
    if (m2sdr_sync_config(dev, M2SDR_TX, format,
                          0, samples_per_buf, 0, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_config failed\n");
        m2sdr_close(dev);
        return 1;
    }

    const double freq = 10000.0;
    const double rate = (double)cfg.sample_rate;
    const double step = 2.0 * M_PI * freq / rate;
    double phase = 0.0;

    uint8_t buf[DMA_BUFFER_SIZE];
    for (int b = 0; b < 100; b++) {
        if (format == M2SDR_FORMAT_SC8_Q7) {
            int8_t *p = (int8_t *)buf;
            for (unsigned i = 0; i < samples_per_buf; i++) {
                int8_t v = (int8_t)(sin(phase) * 127);
                p[2 * i + 0] = v;
                p[2 * i + 1] = v;
                phase += step;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            }
        } else {
            int16_t *p = (int16_t *)buf;
            for (unsigned i = 0; i < samples_per_buf; i++) {
                int16_t v = (int16_t)(sin(phase) * 2047);
                p[2 * i + 0] = v;
                p[2 * i + 1] = v;
                phase += step;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            }
        }
        if (m2sdr_sync_tx(dev, buf, samples_per_buf, NULL, 1000) != 0) {
            fprintf(stderr, "m2sdr_sync_tx failed\n");
            m2sdr_close(dev);
            return 1;
        }
    }

    m2sdr_close(dev);
    return 0;
}
