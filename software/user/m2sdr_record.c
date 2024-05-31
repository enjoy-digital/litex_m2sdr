/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR I/Q Record Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
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

/* Record (DMA RX) */
/*-----------------*/

static void m2sdr_record(const char *device_name, const char *filename, uint32_t size, uint8_t zero_copy)
{
    static struct litepcie_dma_ctrl dma = {.use_writer = 1};

    FILE * fo = NULL;
    int i = 0;
    size_t len;
    size_t total_len = 0;
    int64_t last_time;
    int64_t writer_sw_count_last = 0;

    /* Open File to write to. */
    if (filename != NULL) {
        fo = fopen(filename, "wb");
        if (!fo) {
            perror(filename);
            exit(1);
        }
    }

    /* Initialize DMA. */
    if (litepcie_dma_init(&dma, device_name, zero_copy))
        exit(1);

    dma.writer_enable = 1;

    /* Test Loop. */
    last_time = get_time_ms();
    for (;;) {
        /* Exit loop on CTRL+C. */
        if (!keep_running)
            break;

        /* Update DMA status. */
        litepcie_dma_process(&dma);

        /* Read from DMA. */
        while (1) {
            /* Get Read buffer. */
            char *buf_rd = litepcie_dma_next_read_buffer(&dma);
            /* Break when no buffer available for Read. */
            if (!buf_rd)
                break;
            /* Copy Read data to File. */
            if (filename != NULL) {
                len = fwrite(buf_rd, 1, fmin(size - total_len, DMA_BUFFER_SIZE), fo);
                total_len += len;
            }
            /* Stop when specified size is reached */
            if (size > 0 && total_len >= size)
                keep_running = 0;
        }

        /* Statistics every 200ms. */
        int64_t duration = get_time_ms() - last_time;
        if (duration > 200) {
            /* Print banner every 10 lines. */
            if (i % 10 == 0)
                printf("\e[1mSPEED(Gbps)    BUFFERS SIZE(MB)\e[0m\n");
            i++;
            /* Print statistics. */
            printf("%10.2f %10" PRIu64 "  %8" PRIu64"\n",
                    (double)(dma.writer_sw_count - writer_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6),
                    dma.writer_sw_count,
                    (size > 0) ? ((dma.writer_sw_count) * DMA_BUFFER_SIZE) / 1024 / 1024 : 0);
            /* Update time/count. */
            last_time = get_time_ms();
            writer_sw_count_last = dma.writer_sw_count;
        }
    }

    /* Cleanup DMA. */
    litepcie_dma_cleanup(&dma);

    /* Close File. */
    if (filename != NULL)
        fclose(fo);
}
/* Help */
/*------*/

static void help(void)
{
    printf("LitePCIe testing utilities\n"
           "usage: litepcie_test [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "-h                               Help.\n"
           "-c device_num                    Select the device (default = 0).\n"
           "-z                               Enable zero-copy DMA mode.\n"
           "\n"
           "[filename] [size]                Record I/Q samples stream to file.\n"
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

    litepcie_device_num = 0;
    litepcie_device_zero_copy = 0;

    signal(SIGINT, intHandler);

    /* Parameters. */
    for (;;) {
        c = getopt(argc, argv, "hc:z");
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        case 'z':
            litepcie_device_zero_copy = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/m2sdr%d", litepcie_device_num);

    /* Interpret cmd and record. */
    const char *filename = NULL;
    uint32_t size = 0;
    if (optind != argc) {
        if (optind + 2 > argc)
            goto show_help;
        filename = argv[optind++];
        size = strtoul(argv[optind++], NULL, 0);
    }
    m2sdr_record(litepcie_device, filename, size, litepcie_device_zero_copy);
    return 0;

show_help:
        help();

    return 0;
}
