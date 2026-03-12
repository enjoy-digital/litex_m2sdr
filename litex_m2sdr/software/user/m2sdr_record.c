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
#include <stdbool.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_sigmf.h"
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
           "      --sigmf           Write a SigMF dataset + metadata pair.\n"
           "      --enable-header   Enable DMA header.\n"
           "      --strip-header    Strip DMA header from output.\n"
           "      --format FMT      Sample format: sc16 or sc8 (default: sc16).\n"
           "      --sample-rate HZ  SigMF sample rate metadata.\n"
           "      --center-freq HZ  SigMF center frequency metadata.\n"
           "      --nchannels N     SigMF channel count metadata.\n"
           "      --description TXT SigMF description metadata.\n"
           "      --author TXT      SigMF author metadata.\n"
           "      --hw TXT          SigMF hardware metadata.\n"
           "      --annotation-label TXT   SigMF annotation label.\n"
           "      --annotation-comment TXT SigMF annotation comment.\n"
           "      --zero-copy       Legacy compatibility flag; ignored in sync API.\n"
           "      --8bit            Legacy alias for --format sc8.\n"
           "\n"
           "Arguments:\n"
           "  filename     Output file, or '-' for stdout.\n"
           "  size         Optional byte limit to record.\n");
    exit(1);
}

static void sigmf_datetime_from_ns(uint64_t ts_ns, char *buf, size_t buf_len)
{
    time_t seconds = (time_t)(ts_ns / 1000000000ULL);
    uint32_t nanos = (uint32_t)(ts_ns % 1000000000ULL);
    struct tm tm;

    gmtime_r(&seconds, &tm);
    snprintf(buf, buf_len, "%04d-%02d-%02dT%02d:%02d:%02d.%09uZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, nanos);
}

static void sigmf_fill_defaults_from_device(struct m2sdr_dev *dev, struct m2sdr_sigmf_meta *sigmf_meta)
{
    struct m2sdr_devinfo info;
    enum {
        HW_SAFE_ID = 72,
        HW_SAFE_SERIAL = 32,
        HW_SAFE_TRANSPORT = 16,
        HW_SAFE_PATH = 64,
        DESC_SAFE_PATH = 96,
        DESC_SAFE_TRANSPORT = 16
    };

    if (!dev || !sigmf_meta)
        return;
    if (m2sdr_get_device_info(dev, &info) != 0)
        return;

    if (!sigmf_meta->hw[0]) {
        if (info.identification[0] && info.serial[0]) {
            snprintf(sigmf_meta->hw, sizeof(sigmf_meta->hw), "%.*s serial=%.*s",
                     HW_SAFE_ID, info.identification, HW_SAFE_SERIAL, info.serial);
        } else if (info.identification[0]) {
            snprintf(sigmf_meta->hw, sizeof(sigmf_meta->hw), "%.*s", HW_SAFE_ID, info.identification);
        } else if (info.transport[0] && info.path[0]) {
            snprintf(sigmf_meta->hw, sizeof(sigmf_meta->hw), "LiteX M2SDR (%.*s %.*s)",
                     HW_SAFE_TRANSPORT, info.transport, HW_SAFE_PATH, info.path);
        }
    }

    if (!sigmf_meta->description[0]) {
        if (info.transport[0] && info.path[0]) {
            snprintf(sigmf_meta->description, sizeof(sigmf_meta->description),
                     "Capture recorded with m2sdr_record on %.*s via %.*s",
                     DESC_SAFE_PATH, info.path, DESC_SAFE_TRANSPORT, info.transport);
        } else {
            snprintf(sigmf_meta->description, sizeof(sigmf_meta->description),
                     "Capture recorded with m2sdr_record");
        }
    }
}

static void m2sdr_record(const char *device_id, const char *filename, size_t size, uint8_t quiet,
                         uint8_t header, uint8_t strip_header, enum m2sdr_format format,
                         bool sigmf_enable, struct m2sdr_sigmf_meta *sigmf_meta)
{
    struct m2sdr_dev *dev = NULL;
    char sigmf_data_path[1024] = {0};
    char sigmf_meta_path[1024] = {0};
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

    if (sigmf_enable)
        sigmf_fill_defaults_from_device(dev, sigmf_meta);

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
    uint64_t first_timestamp = 0;

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
        if (first_timestamp == 0 && header && (meta.flags & M2SDR_META_FLAG_HAS_TIME))
            first_timestamp = meta.timestamp;

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

    if (sigmf_enable) {
        uint64_t total_samples = sample_size ? (uint64_t)(total_len / sample_size) : 0;

        if (m2sdr_sigmf_derive_paths(filename, sigmf_data_path, sizeof(sigmf_data_path),
                                     sigmf_meta_path, sizeof(sigmf_meta_path)) != 0) {
            fprintf(stderr, "Could not derive SigMF paths from %s\n", filename);
        } else {
            snprintf(sigmf_meta->data_path, sizeof(sigmf_meta->data_path), "%s", sigmf_data_path);
            snprintf(sigmf_meta->meta_path, sizeof(sigmf_meta->meta_path), "%s", sigmf_meta_path);
            if (sigmf_meta->annotation_count > 0) {
                sigmf_meta->annotations[0].sample_start = 0;
                sigmf_meta->annotations[0].sample_count = total_samples;
                sigmf_meta->annotations[0].has_sample_count = total_samples > 0;
            }
            if (first_timestamp != 0) {
                sigmf_datetime_from_ns(first_timestamp, sigmf_meta->datetime, sizeof(sigmf_meta->datetime));
                sigmf_meta->has_datetime = true;
            }
            if (m2sdr_sigmf_write(sigmf_meta) != 0)
                fprintf(stderr, "Failed to write SigMF metadata: %s\n", sigmf_meta_path);
        }
    }

    m2sdr_close(dev);
}

int main(int argc, char **argv)
{
    int c;
    int option_index = 0;
    static uint8_t quiet = 0;
    static uint8_t sigmf_enable = 0;
    static uint8_t header = 0;
    static uint8_t strip_header = 0;
    static enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    struct m2sdr_sigmf_meta sigmf_meta;
    struct m2sdr_cli_device cli_dev;
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "quiet", no_argument, NULL, 'q' },
        { "sigmf", no_argument, NULL, 'm' },
        { "zero-copy", no_argument, NULL, 'z' },
        { "enable-header", no_argument, NULL, 'H' },
        { "strip-header", no_argument, NULL, 's' },
        { "sample-rate", required_argument, NULL, 3 },
        { "center-freq", required_argument, NULL, 4 },
        { "nchannels", required_argument, NULL, 5 },
        { "description", required_argument, NULL, 6 },
        { "author", required_argument, NULL, 7 },
        { "hw", required_argument, NULL, 8 },
        { "annotation-label", required_argument, NULL, 9 },
        { "annotation-comment", required_argument, NULL, 10 },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);
    memset(&sigmf_meta, 0, sizeof(sigmf_meta));
    snprintf(sigmf_meta.recorder, sizeof(sigmf_meta.recorder), "m2sdr_record");

    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:p:zqHsm", options, &option_index);
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
        case 'm':
            sigmf_enable = 1;
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
        case 3:
            sigmf_meta.sample_rate = atof(optarg);
            sigmf_meta.has_sample_rate = true;
            break;
        case 4:
            sigmf_meta.center_freq = atof(optarg);
            sigmf_meta.has_center_freq = true;
            break;
        case 5:
            sigmf_meta.num_channels = (unsigned)strtoul(optarg, NULL, 0);
            sigmf_meta.has_num_channels = true;
            break;
        case 6:
            snprintf(sigmf_meta.description, sizeof(sigmf_meta.description), "%s", optarg);
            break;
        case 7:
            snprintf(sigmf_meta.author, sizeof(sigmf_meta.author), "%s", optarg);
            break;
        case 8:
            snprintf(sigmf_meta.hw, sizeof(sigmf_meta.hw), "%s", optarg);
            break;
        case 9:
            sigmf_meta.annotation_count = 1;
            snprintf(sigmf_meta.annotations[0].label, sizeof(sigmf_meta.annotations[0].label), "%s", optarg);
            break;
        case 10:
            sigmf_meta.annotation_count = 1;
            snprintf(sigmf_meta.annotations[0].comment, sizeof(sigmf_meta.annotations[0].comment), "%s", optarg);
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

    if (sigmf_enable) {
        const char *datatype = m2sdr_sigmf_datatype_from_format(format);
        if (!filename || strcmp(filename, "-") == 0) {
            fprintf(stderr, "SigMF output requires a file path, not stdout\n");
            return 1;
        }
        if (header && !strip_header) {
            fprintf(stderr, "SigMF MVP currently requires --strip-header when --enable-header is used\n");
            return 1;
        }
        if (!datatype) {
            fprintf(stderr, "Unsupported SigMF datatype for selected format\n");
            return 1;
        }
        snprintf(sigmf_meta.datatype, sizeof(sigmf_meta.datatype), "%s", datatype);
        if (!sigmf_meta.has_num_channels) {
            sigmf_meta.num_channels = 2;
            sigmf_meta.has_num_channels = true;
        }
        if (header && strip_header) {
            sigmf_meta.header_bytes = 0;
            sigmf_meta.has_header_bytes = false;
        }
    }

    m2sdr_record(m2sdr_cli_device_id(&cli_dev), filename, size, quiet, header, strip_header, format,
                 sigmf_enable ? true : false, &sigmf_meta);
    return 0;
}
