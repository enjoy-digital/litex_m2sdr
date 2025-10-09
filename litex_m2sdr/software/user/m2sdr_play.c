/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR I/Q Player Utility.
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
/* Play (DMA TX) */
/*---------------*/

static void m2sdr_play(const char *device_name, const char *filename, uint32_t loops, uint8_t zero_copy, uint8_t quiet, uint8_t timed_start)
{
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    FILE *fo;
    int close_fo = 0;
    int i = 0;
    size_t len;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint32_t current_loop = 0;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;

    /* Open File or use stdin */
    if (strcmp(filename, "-") == 0) {
        fo = stdin;
    } else {
        fo = fopen(filename, "rb");
        if (!fo) {
            perror(filename);
            exit(1);
        }
        close_fo = 1;
    }

    /* Initialize DMA. */
    if (litepcie_dma_init(&dma, device_name, zero_copy))
        exit(1);

    if (timed_start) {
        /* Read initial timestamp */
        uint32_t ctrl = m2sdr_readl(dma.fds.fd, CSR_TIME_GEN_CONTROL_ADDR);
        m2sdr_writel(dma.fds.fd, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
        m2sdr_writel(dma.fds.fd, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);
        uint64_t current_ts = (
            ((uint64_t) m2sdr_readl(dma.fds.fd, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32 |
            ((uint64_t) m2sdr_readl(dma.fds.fd, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0
        );
        time_t seconds = current_ts / 1000000000ULL;
        uint32_t ms = (current_ts % 1000000000ULL) / 1000000;
        struct tm tm;
        localtime_r(&seconds, &tm);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
        printf("Initial Time : %s.%03u\n", time_str, ms);

        /* Calculate next full second */
        uint64_t ns_in_sec = current_ts % 1000000000ULL;
        uint64_t wait_ns   = (ns_in_sec == 0) ? 1000000000ULL : (1000000000ULL - ns_in_sec);
        uint64_t target_ts = current_ts + wait_ns;

        /* Poll until target time reached */
        uint64_t poll_ts;
        do {
            ctrl = m2sdr_readl(dma.fds.fd, CSR_TIME_GEN_CONTROL_ADDR);
            m2sdr_writel(dma.fds.fd, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
            m2sdr_writel(dma.fds.fd, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);
            poll_ts = (
                ((uint64_t) m2sdr_readl(dma.fds.fd, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32 |
                ((uint64_t) m2sdr_readl(dma.fds.fd, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0
            );
            usleep(1000); /* 1ms poll */
        } while (poll_ts < target_ts);
        usleep(100000); /* 100ms delay */

        /* Display DMA start time */
        target_ts += 1000000000ULL;
        seconds = target_ts / 1000000000ULL;
        ms      = (target_ts % 1000000000ULL) / 1000000;
        localtime_r(&seconds, &tm);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
        printf("Start Time   : %s.%03u\n", time_str, ms);
    }

    dma.reader_enable = 1;

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
            /* Read data from File and fill Write buffer */
            len = fread(buf_wr, 1, DMA_BUFFER_SIZE, fo);
            if (feof(fo)) {
                /* Rewind on end of file, but not for stdin */
                current_loop += 1;
                if (loops != 0 && current_loop >= loops)
                    keep_running = 0;
                if (strcmp(filename, "-") != 0) {
                    rewind(fo);
                    len += fread(buf_wr + len, 1, DMA_BUFFER_SIZE - len, fo);
                }
            }
        }

        /* Statistics every 200ms. */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            /* Print banner every 10 lines. */
            if (i % 10 == 0)
                printf("\e[1mSPEED(Gbps)   BUFFERS   SIZE(MB)   LOOP UNDERFLOWS\e[0m\n");
            i++;
            /* Print statistics. */
            printf("%10.2f %10" PRIu64 " %10" PRIu64 " %6d %10ld\n",
                   (double)(dma.reader_sw_count - reader_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6),
                   dma.reader_sw_count,
                   (dma.reader_sw_count * DMA_BUFFER_SIZE) / 1024 / 1024,
                   current_loop,
                   sw_underflows);
            /* Update time/count/underflows. */
            last_time = get_time_ms();
            reader_sw_count_last = dma.reader_hw_count;
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

    /* Close File if not stdin */
    if (close_fo)
        fclose(fo);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR I/Q Player Utility\n"
           "usage: m2sdr_play [options] filename [loops]\n"
           "\n"
           "Options:\n"
           "-h                    Display this help message.\n"
           "-c device_num         Select the device (default = 0).\n"
           "-z                    Enable zero-copy DMA mode.\n"
           "-q                    Quiet mode (suppress statistics).\n"
           "-t                    Timed start mode.\n"
           "\n"
           "Arguments:\n"
           "filename              I/Q samples stream file to play.\n"
           "loops                 Number of times to loop playback (default=1, 0 for infinite).\n");
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
    static uint8_t quiet = 0;
    static uint8_t timed_start = 0;
    m2sdr_device_num = 0;
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
            timed_start = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);

    /* Interpret cmd and play. */
    const char *filename;
    uint32_t loops = 1;
    if (optind < argc) {
        filename = argv[optind++];
        if (strcmp(filename, "-") == 0) {
            loops = 1;
        } else if (optind < argc) {
            loops = strtoul(argv[optind++], NULL, 0);
        }
    } else if (!isatty(STDIN_FILENO)) {
        filename = "-";
        loops = 1;
    } else {
        help();
    }
    m2sdr_play(m2sdr_device, filename, loops, m2sdr_device_zero_copy, quiet, timed_start);
    return 0;
}