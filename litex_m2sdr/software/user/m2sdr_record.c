/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR I/Q Record Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>

#include "m2sdr.h"
#include "config.h"

#include "litepcie_helpers.h"

#if defined(USE_LITEETH)
static char m2sdr_ip_address[1024] = "192.168.1.50";
static char m2sdr_port[16] = "1234";
#endif

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

static void help(void)
{
    printf("M2SDR I/Q Record Utility.\n"
           "usage: m2sdr_record [options] [filename|-] [size]\n"
           "\n"
           "Options:\n"
           "  -h           Show this help message.\n"
#ifdef USE_LITEPCIE
           "  -c N         Use /dev/m2sdrN (default: 0).\n"
#elif defined(USE_LITEETH)
           "  -i IP        Target IP address (default: 192.168.1.50).\n"
           "  -p PORT      Target port (default: 1234).\n"
#endif
           "  -z           Zero-copy (accepted, ignored in sync API).\n"
           "  -q           Quiet mode.\n"
           "  -H           Enable header (not supported in sync API).\n"
           "  -s           Strip header (ignored).\n"
           "\n"
           "Arguments:\n"
           "  filename     Output file, or '-' for stdout.\n"
           "  size         Optional byte limit to record.\n");
    exit(1);
}

static void m2sdr_record(const char *device_id, const char *filename, size_t size, uint8_t quiet, uint8_t header, uint8_t strip_header)
{
    if (header || strip_header)
        fprintf(stderr, "Header options are not supported in sync API; ignoring.\n");

    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }

    unsigned samples_per_buf = DMA_BUFFER_SIZE / 4;
    if (m2sdr_sync_config(dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11,
                          0, samples_per_buf, 0, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_config failed\n");
        m2sdr_close(dev);
        exit(1);
    }

    FILE *fo = NULL;
    if (filename != NULL) {
        if (strcmp(filename, "-") == 0) {
            fo = stdout;
        } else {
            fo = fopen(filename, "wb");
            if (!fo) {
                perror(filename);
                m2sdr_close(dev);
                exit(1);
            }
        }
    }

    int i = 0;
    size_t total_len = 0;
    int64_t last_time = get_time_ms();
    uint64_t total_buffers = 0;
    uint64_t last_buffers  = 0;

    uint8_t buf[DMA_BUFFER_SIZE];
    while (keep_running) {
        if (size > 0 && total_len >= size)
            break;

        if (m2sdr_sync_rx(dev, buf, samples_per_buf, NULL, 0) != 0) {
            fprintf(stderr, "m2sdr_sync_rx failed\n");
            break;
        }

        size_t to_write = DMA_BUFFER_SIZE;
        if (size > 0 && to_write > size - total_len)
            to_write = size - total_len;

        if (fo) {
            if (fwrite(buf, 1, to_write, fo) != to_write) {
                perror("fwrite");
                break;
            }
        }
        total_len += to_write;
        total_buffers++;

        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            double speed  = (double)(total_buffers - last_buffers) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6);
            uint64_t size_mb = (total_buffers * DMA_BUFFER_SIZE) / 1024 / 1024;

            if (i % 10 == 0)
                fprintf(stderr, "\e[1m%10s %10s %9s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)");
            i++;

            fprintf(stderr, "%11.2f %10" PRIu64 " %9" PRIu64 "\n", speed, total_buffers, size_mb);

            last_time = get_time_ms();
            last_buffers = total_buffers;
        }
    }

    if (fo && fo != stdout)
        fclose(fo);
    m2sdr_close(dev);
}

int main(int argc, char **argv)
{
    int c;
    static int m2sdr_device_num = 0;
    static uint8_t quiet = 0;
    static uint8_t header = 0;
    static uint8_t strip_header = 0;

    signal(SIGINT, intHandler);

    for (;;) {
#if defined(USE_LITEPCIE)
        c = getopt(argc, argv, "hc:zqHs");
#elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:zqHs");
#endif
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
#if defined(USE_LITEPCIE)
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
#elif defined(USE_LITEETH)
        case 'i':
            strncpy(m2sdr_ip_address, optarg, sizeof(m2sdr_ip_address) - 1);
            m2sdr_ip_address[sizeof(m2sdr_ip_address) - 1] = '\0';
            break;
        case 'p':
            strncpy(m2sdr_port, optarg, sizeof(m2sdr_port) - 1);
            m2sdr_port[sizeof(m2sdr_port) - 1] = '\0';
            break;
#endif
        case 'z':
            break;
        case 'q':
            quiet = 1;
            break;
        case 'H':
            header = 1;
            break;
        case 's':
            strip_header = 1;
            break;
        default:
            exit(1);
        }
    }

    const char *filename = NULL;
    size_t size = 0;
    if (optind < argc) {
        filename = argv[optind++];
        if (optind < argc)
            size = strtoull(argv[optind++], NULL, 0);
    } else if (!isatty(STDIN_FILENO)) {
        filename = "-";
    }

    char device_id[128];
#ifdef USE_LITEPCIE
    snprintf(device_id, sizeof(device_id), "pcie:/dev/m2sdr%d", m2sdr_device_num);
#elif defined(USE_LITEETH)
    snprintf(device_id, sizeof(device_id), "eth:%s:%s", m2sdr_ip_address, m2sdr_port);
#endif

    m2sdr_record(device_id, filename, size, quiet, header, strip_header);
    return 0;
}
