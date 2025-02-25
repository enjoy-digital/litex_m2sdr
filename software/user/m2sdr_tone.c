/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Tone Generator Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>

#include "liblitepcie.h"

/* Variables */
/*-----------*/

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* Tone (DMA TX) */
/*---------------*/

static void m2sdr_tone(const char *device_name, double sample_rate, double frequency, double amplitude, uint8_t zero_copy)
{
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;

    /* Print parameters */
    printf("Starting tone generation with parameters:\n");
    printf("  Device: %s\n", device_name);
    printf("  Sample Rate: %.0f Hz\n", sample_rate);
    printf("  Frequency: %.0f Hz\n", frequency);
    printf("  Amplitude: %.2f\n", amplitude);
    printf("  Zero-Copy Mode: %d\n", zero_copy);

    /* Initialize DMA. */
    if (litepcie_dma_init(&dma, device_name, zero_copy))
        exit(1);

    dma.reader_enable = 1;

    /* Tone generation parameters */
    double phi = 0.0;
    double omega = 2 * M_PI * frequency / sample_rate;

    /* Test Loop. */
    last_time = get_time_ms();
    for (;;) {
        /* Update DMA status. */
        litepcie_dma_process(&dma);

        /* Exit loop on CTRL+C. */
        if (!(keep_running)) {
            hw_count_stop = dma.reader_sw_count + 16;
            break;
        }

        /* Write to DMA. */
        while (1) {
            /* Get Write buffer. */
            char *buf_wr = litepcie_dma_next_write_buffer(&dma);
            /* Break when no buffer available for Write */
            if (!buf_wr) {
                break;
            }
            /* Detect DMA underflows. */
            if (dma.reader_sw_count - dma.reader_hw_count < 0)
                sw_underflows += (dma.reader_hw_count - dma.reader_sw_count);

            /* Generate tone and fill Write buffer */
            int num_samples = DMA_BUFFER_SIZE / 8; // 8 bytes per sample: TX1_I, TX1_Q, TX2_I, TX2_Q
            for (int j = 0; j < num_samples; j++) {
                float I = cos(phi) * amplitude;
                float Q = sin(phi) * amplitude;
                int16_t I_int = (int16_t)(I * 2047); // Scale to 12-bit range
                int16_t Q_int = (int16_t)(Q * 2047); // Scale to 12-bit range

                ((int16_t *)buf_wr)[4 * j + 0] = I_int; // TX1_I
                ((int16_t *)buf_wr)[4 * j + 1] = Q_int; // TX1_Q
                ((int16_t *)buf_wr)[4 * j + 2] = I_int; // TX2_I
                ((int16_t *)buf_wr)[4 * j + 3] = Q_int; // TX2_Q

                phi += omega;
                if (phi >= 2 * M_PI) phi -= 2 * M_PI;
            }
        }

        /* Statistics every 200ms. */
        int64_t duration = get_time_ms() - last_time;
        if (duration > 200) {
            /* Print banner every 10 lines. */
            if (i % 10 == 0)
                printf("\e[1mSPEED(Gbps)   BUFFERS   SIZE(MB)   UNDERFLOWS\e[0m\n");
            i++;
            /* Print statistics. */
            printf("%10.2f %10" PRIu64 " %10" PRIu64 " %10ld\n",
                   (double)(dma.reader_sw_count - reader_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6),
                   dma.reader_sw_count,
                   (dma.reader_sw_count * DMA_BUFFER_SIZE) / 1024 / 1024,
                   sw_underflows);
            /* Update time/count/underflows. */
            last_time = get_time_ms();
            reader_sw_count_last = dma.reader_sw_count;
            sw_underflows = 0;
        }
    }

    /* Wait end of DMA transfer. */
    while (dma.reader_hw_count < hw_count_stop) {
        dma.reader_enable = 1;
        litepcie_dma_process(&dma);
    }

    /* Cleanup DMA. */
    litepcie_dma_cleanup(&dma);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Tone Generator Utility\n"
           "usage: m2sdr_tone [options]\n"
           "\n"
           "options:\n"
           "-h                               Help.\n"
           "-c device_num                    Select the device (default = 0).\n"
           "-s sample_rate                   Set sample rate in Hz (default = 30720000).\n"
           "-f frequency                     Set tone frequency in Hz (default = 1000).\n"
           "-a amplitude                     Set amplitude (0.0 to 1.0, default = 1.0).\n"
           "-z                               Enable zero-copy DMA mode.\n"
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;
    static char litepcie_device[1024];
    static int litepcie_device_num;
    static uint8_t litepcie_device_zero_copy;
    double sample_rate = 30720000.0;
    double frequency = 1000.0;
    double amplitude = 1.0;

    litepcie_device_num = 0;
    litepcie_device_zero_copy = 0;

    signal(SIGINT, intHandler);

    /* Parameters. */
    for (;;) {
        c = getopt(argc, argv, "hc:s:f:a:z");
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        case 's':
            sample_rate = atof(optarg);
            if (sample_rate <= 0) {
                fprintf(stderr, "Sample rate must be positive\n");
                exit(1);
            }
            break;
        case 'f':
            frequency = atof(optarg);
            if (frequency < 0) {
                fprintf(stderr, "Frequency must be non-negative\n");
                exit(1);
            }
            break;
        case 'a':
            amplitude = atof(optarg);
            if (amplitude < 0.0 || amplitude > 1.0) {
                fprintf(stderr, "Amplitude must be between 0.0 and 1.0\n");
                exit(1);
            }
            break;
        case 'z':
            litepcie_device_zero_copy = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Show help when no args. */
    if (optind != argc)
        help();

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/m2sdr%d", litepcie_device_num);

    /* Generate and play tone. */
    m2sdr_tone(litepcie_device, sample_rate, frequency, amplitude, litepcie_device_zero_copy);

    return 0;
}