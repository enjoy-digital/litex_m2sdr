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
#include <time.h>
#include <getopt.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "config.h"

#include "litepcie_helpers.h"

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
           "  -h, --help            Show this help message.\n"
           "  -d, --device DEV      Use explicit device id.\n"
#ifdef USE_LITEPCIE
           "  -c, --device-num N    Use /dev/m2sdrN (default: 0).\n"
#elif defined(USE_LITEETH)
           "  -i, --ip ADDR         Target IP address (default: 192.168.1.50).\n"
           "  -p, --port PORT       Target port (default: 1234).\n"
#endif
           "  -q, --quiet           Quiet mode.\n"
           "      --enable-header   Enable DMA header.\n"
           "      --strip-header    Strip DMA header from output.\n"
           "      --format FMT      Sample format: sc16 or sc8 (default: sc16).\n"
           "      --zero-copy       Legacy compatibility flag; ignored in sync API.\n"
           "      --8bit            Legacy alias for --format sc8.\n"
           "\n"
           "Arguments:\n"
           "  filename     Output file, or '-' for stdout.\n"
           "  size         Optional byte limit to record.\n");
    exit(1);
}

static void m2sdr_record(const char *device_id, const char *filename, size_t size, uint8_t quiet, uint8_t header, uint8_t strip_header, enum m2sdr_format format)
{
    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }

    if (header) {
        if (m2sdr_set_rx_header(dev, true, strip_header ? true : false) != 0) {
            fprintf(stderr, "m2sdr_set_rx_header failed\n");
            m2sdr_close(dev);
            exit(1);
        }
    }

    unsigned sample_size = m2sdr_format_size(format);
    unsigned samples_per_buf = DMA_BUFFER_SIZE / sample_size;
    size_t payload_bytes_per_buf = DMA_BUFFER_SIZE;
    if (header && strip_header)
        payload_bytes_per_buf = DMA_BUFFER_SIZE - 16;
    if (header && strip_header)
        samples_per_buf = (DMA_BUFFER_SIZE - 16) / sample_size;
    if (m2sdr_sync_config(dev, M2SDR_RX, format,
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
    uint64_t last_timestamp = 0;

    uint8_t buf[DMA_BUFFER_SIZE];
    while (keep_running) {
        struct m2sdr_metadata meta;
        if (size > 0 && total_len >= size)
            break;

        if (m2sdr_sync_rx(dev, buf, samples_per_buf, header ? &meta : NULL, 0) != 0) {
            fprintf(stderr, "m2sdr_sync_rx failed\n");
            break;
        }
        if (header && (meta.flags & M2SDR_META_FLAG_HAS_TIME))
            last_timestamp = meta.timestamp;

        size_t to_write = payload_bytes_per_buf;
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

            if (i % 10 == 0) {
                if (header) {
                    fprintf(stderr, "\e[1m%10s %10s %8s %23s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)", "TIME");
                } else {
                    fprintf(stderr, "\e[1m%10s %10s %9s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)");
                }
            }
            i++;

            if (header && last_timestamp != 0) {
                uint64_t seconds = last_timestamp / 1000000000ULL;
                uint64_t nanos   = last_timestamp % 1000000000ULL;
                uint32_t ms      = nanos / 1000000;
                time_t sec_time  = (time_t)seconds;
                struct tm tm;
                char time_str[64];

                gmtime_r(&sec_time, &tm);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
                fprintf(stderr, "%11.2f %10" PRIu64 " %8" PRIu64 " %19s.%03u\n",
                        speed, total_buffers, size_mb, time_str, ms);
            } else {
                fprintf(stderr, "%11.2f %10" PRIu64 " %9" PRIu64 "\n", speed, total_buffers, size_mb);
            }

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
    int option_index = 0;
    static uint8_t quiet = 0;
    static uint8_t header = 0;
    static uint8_t strip_header = 0;
    static enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    struct m2sdr_cli_device cli_dev;
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "quiet", no_argument, NULL, 'q' },
        { "zero-copy", no_argument, NULL, 'z' },
        { "enable-header", no_argument, NULL, 'H' },
        { "strip-header", no_argument, NULL, 's' },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);

    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:p:zqHs", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            return 0;
        case 'd':
        case 'c':
        case 'i':
        case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                exit(1);
            break;
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
        case 1:
            if (!strcmp(optarg, "sc16")) {
                format = M2SDR_FORMAT_SC16_Q11;
            } else if (!strcmp(optarg, "sc8")) {
                format = M2SDR_FORMAT_SC8_Q7;
            } else {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case 2:
            format = M2SDR_FORMAT_SC8_Q7;
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

    if (!m2sdr_cli_finalize_device(&cli_dev))
        return 1;

    m2sdr_record(m2sdr_cli_device_id(&cli_dev), filename, size, quiet, header, strip_header, format);
    return 0;
}
