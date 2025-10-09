/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR I/Q Record Utility.
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
#include <time.h>

#include "liblitepcie.h"
#include "libm2sdr.h"

/* Variables */
/*-----------*/

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* Record (DMA RX) */
/*-----------------*/

static void m2sdr_record(const char *device_name, const char *filename, size_t size, uint8_t zero_copy, uint8_t quiet, uint8_t header)
{
    static const uint64_t DMA_HEADER_SYNC_WORD = 0x5aa55aa55aa55aa5ULL;
    static struct litepcie_dma_ctrl dma = {.use_writer = 1};

    FILE *fo = NULL;
    int i = 0;
    size_t len;
    size_t total_len = 0;
    int64_t last_time;
    int64_t writer_sw_count_last = 0;
    uint64_t last_timestamp = 0;

    /* Open File or use stdout */
    if (filename != NULL) {
        if (strcmp(filename, "-") == 0) {
            fo = stdout;
        } else {
            fo = fopen(filename, "wb");
            if (!fo) {
                perror(filename);
                exit(1);
            }
        }
    }

    /* Initialize DMA. */
    if (litepcie_dma_init(&dma, device_name, zero_copy))
        exit(1);
    dma.writer_enable = 1;

    /* Configure RX Header */
    m2sdr_writel(dma.fds.fd, CSR_HEADER_RX_CONTROL_ADDR,
       (1      << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
       (header << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)
    );

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

            /* Extract timestamp if header enabled */
            if (header) {
                uint64_t sync = *(uint64_t*)buf_rd;
                if (sync == DMA_HEADER_SYNC_WORD) {
                    uint64_t timestamp = *(uint64_t*)(buf_rd + 8);
                    last_timestamp = timestamp;
                }
            }

            /* Copy Read data to File or stdout. */
            if (fo != NULL) {
                if (size > 0 && total_len >= size) {
                    keep_running = 0;
                    break;
                }
                size_t to_write = DMA_BUFFER_SIZE;
                if (size > 0 && to_write > size - total_len) {
                    to_write = size - total_len;
                }
                len = fwrite(buf_rd, 1, to_write, fo);
                total_len += len;
            }
        }

        /* Statistics every 200ms. */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            double speed     = (double)(dma.writer_sw_count - writer_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6);
            uint64_t buffers = dma.writer_sw_count;
            uint64_t size_mb = ((dma.writer_sw_count) * DMA_BUFFER_SIZE) / 1024 / 1024;

            /* Print banner every 10 lines. */
            if (i % 10 == 0) {
                if (header) {
                    fprintf(stderr, "\e[1m%10s %10s %8s %23s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)", "TIME");
                } else {
                    fprintf(stderr, "\e[1m%10s %10s %9s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)");
                }
            }
            i++;

            /* Print statistics. */
            if (header) {
                uint64_t seconds = last_timestamp / 1000000000ULL;
                uint64_t nanos   = last_timestamp % 1000000000ULL;
                uint32_t ms      = nanos / 1000000;
                struct tm tm;
                gmtime_r((const time_t*)&seconds, &tm);
                char time_str[64];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
                fprintf(stderr, "%11.2f %10" PRIu64 " %8" PRIu64 " %19s.%03u\n",
                    speed, buffers, size_mb, time_str, ms);
            } else {
                fprintf(stderr, "%11.2f %10" PRIu64 " %8" PRIu64"\n",
                    speed, buffers, size_mb);
            }
            /* Update time/count. */
            last_time = get_time_ms();
            writer_sw_count_last = dma.writer_sw_count;
        }
    }

    /* Cleanup DMA. */
    litepcie_dma_cleanup(&dma);

    /* Close File if not stdout */
    if (fo != NULL && fo != stdout)
        fclose(fo);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR I/Q Record Utility\n"
           "usage: m2sdr_record [options] [filename] [size]\n"
           "\n"
           "Options:\n"
           "-h                    Display this help message.\n"
           "-c device_num         Select the device (default = 0).\n"
           "-z                    Enable zero-copy DMA mode.\n"
           "-q                    Quiet mode (suppress statistics).\n"
           "-t                    Enable RX Header with timestamp.\n"
           "\n"
           "Arguments:\n"
           "filename              File to record I/Q samples to (optional, omit to monitor stream).\n"
           "size                  Number of samples to record (optional, 0 for infinite).\n");
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;
    static char m2sdr_device[1024];
    static int m2sdr_device_num;
    static uint8_t m2sdr_device_zero_copy;
    static uint8_t quiet   = 0;
    static uint8_t header  = 0;
    m2sdr_device_num       = 0;
    m2sdr_device_zero_copy = 0;

    signal(SIGINT, intHandler);

    /* Parameters. */
    for (;;) {
        c = getopt(argc, argv, "hc:zqt");
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
        case 'z':
            m2sdr_device_zero_copy = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        case 't':
            header = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);

    /* Interpret cmd and record. */
    const char *filename = NULL;
    size_t size = 0;
    if (optind != argc) {
        filename = argv[optind++];  /* Allow filename to be provided or "-" */
        if (optind < argc) {        /* Size is optional */
            size = strtoul(argv[optind++], NULL, 0);
        }
    }
    m2sdr_record(m2sdr_device, filename, size, m2sdr_device_zero_copy, quiet, header);
    return 0;
}
