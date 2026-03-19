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
#include <math.h>

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
    puts("M2SDR I/Q Record Utility.");
    puts("usage: m2sdr_record [options] [filename|-] [size]");
    puts("");
    puts("Arguments:");
    puts("  filename     Output file, or '-' for stdout.");
    puts("  size         Optional byte limit to record.");
    puts("");
    m2sdr_cli_print_device_help();
    puts("");
    puts("Capture options:");
    puts("  -h, --help            Show this help message.");
    puts("  -q, --quiet           Quiet mode.");
    puts("      --format FMT      Sample format: sc16 or sc8 (default: sc16).");
    puts("      --enable-header   Enable DMA header.");
    puts("      --strip-header    Strip DMA header from output.");
    puts("");
    puts("SigMF output:");
    puts("      --sigmf           Write a SigMF dataset + metadata pair.");
    puts("      --sample-rate HZ  Set SigMF sample rate metadata.");
    puts("      --center-freq HZ  Set SigMF center frequency metadata.");
    puts("      --nchannels N     Set SigMF channel count metadata.");
    puts("      --description TXT Set SigMF description metadata.");
    puts("      --author TXT      Set SigMF author metadata.");
    puts("      --hw TXT          Set SigMF hardware metadata.");
    puts("");
    puts("SigMF annotations:");
    puts("      --annotation-label TXT     Set current annotation label.");
    puts("      --annotation-comment TXT   Set current annotation comment.");
    puts("      --annotation-start N       Set current annotation start sample.");
    puts("      --annotation-count N       Set current annotation sample count.");
    puts("      --annotation-freq-low HZ   Set current annotation lower frequency edge.");
    puts("      --annotation-freq-high HZ  Set current annotation upper frequency edge.");
    puts("      --annotation-add           Finalize the current annotation and start a new one.");
    puts("      --annotate-ts-jumps        Add annotations on RX timestamp jumps.");
    puts("      --ts-jump-threshold-pct P  Relative jump threshold (default: 5).");
    puts("");
    puts("Compatibility:");
    puts("      --zero-copy       Legacy compatibility flag; ignored in sync API.");
    puts("      --8bit            Legacy alias for --format sc8.");
    exit(1);
}

static bool sigmf_annotation_has_fields(const struct m2sdr_sigmf_annotation *ann)
{
    if (!ann)
        return false;
    return ann->label[0] || ann->comment[0] || ann->sample_start != 0 || ann->has_sample_count ||
           ann->has_freq_lower_edge || ann->has_freq_upper_edge;
}

static int sigmf_append_annotation(struct m2sdr_sigmf_meta *meta, const struct m2sdr_sigmf_annotation *ann)
{
    if (!meta || !ann || !sigmf_annotation_has_fields(ann))
        return -1;
    if (meta->annotation_count >= M2SDR_SIGMF_MAX_ANNOTATIONS)
        return -1;
    meta->annotations[meta->annotation_count++] = *ann;
    return 0;
}

static int sigmf_finalize_pending_annotation(struct m2sdr_sigmf_meta *meta,
                                             struct m2sdr_sigmf_annotation *pending)
{
    if (!sigmf_annotation_has_fields(pending))
        return 0;
    if (sigmf_append_annotation(meta, pending) != 0)
        return -1;
    memset(pending, 0, sizeof(*pending));
    return 0;
}

static void sigmf_finalize_annotation_ranges(struct m2sdr_sigmf_meta *meta, uint64_t total_samples)
{
    unsigned i;

    if (!meta)
        return;

    for (i = 0; i < meta->annotation_count; i++) {
        struct m2sdr_sigmf_annotation *ann = &meta->annotations[i];

        if (ann->has_sample_count)
            continue;
        if (ann->sample_start >= total_samples) {
            ann->sample_count = 0;
            ann->has_sample_count = true;
            continue;
        }
        ann->sample_count = total_samples - ann->sample_start;
        ann->has_sample_count = true;
    }
}

static void sigmf_record_timestamp_jump(struct m2sdr_sigmf_meta *meta,
                                        uint64_t sample_start,
                                        uint64_t sample_count,
                                        uint64_t dt_ns,
                                        uint64_t expected_dt_ns)
{
    struct m2sdr_sigmf_annotation ann;

    if (!meta || meta->annotation_count >= M2SDR_SIGMF_MAX_ANNOTATIONS)
        return;

    memset(&ann, 0, sizeof(ann));
    ann.sample_start = sample_start;
    ann.sample_count = sample_count;
    ann.has_sample_count = true;
    snprintf(ann.label, sizeof(ann.label), "timestamp-jump");
    snprintf(ann.comment, sizeof(ann.comment), "dt=%" PRIu64 " ns expected=%" PRIu64 " ns", dt_ns, expected_dt_ns);
    sigmf_append_annotation(meta, &ann);
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
                         bool sigmf_enable, bool annotate_ts_jumps, double ts_jump_threshold_pct,
                         struct m2sdr_sigmf_meta *sigmf_meta)
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
    uint64_t prev_timestamp = 0;
    uint64_t nominal_dt = 0;

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
        if (sigmf_enable && annotate_ts_jumps && header && (meta.flags & M2SDR_META_FLAG_HAS_TIME)) {
            if (prev_timestamp != 0) {
                uint64_t dt_ns = meta.timestamp - prev_timestamp;

                if (nominal_dt == 0) {
                    nominal_dt = dt_ns;
                } else if (m2sdr_sigmf_timestamp_jump_is_anomalous(nominal_dt, dt_ns, ts_jump_threshold_pct)) {
                    uint64_t sample_start = sample_size ? (uint64_t)(total_len / sample_size) : 0;
                    sigmf_record_timestamp_jump(sigmf_meta, sample_start, samples_per_buf, dt_ns, nominal_dt);
                }
            }
            prev_timestamp = meta.timestamp;
        }

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
            sigmf_finalize_annotation_ranges(sigmf_meta, total_samples);
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
    static uint8_t annotate_ts_jumps = 0;
    static enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    double ts_jump_threshold_pct = 5.0;
    struct m2sdr_sigmf_meta sigmf_meta;
    struct m2sdr_sigmf_annotation pending_annotation;
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
        { "annotation-start", required_argument, NULL, 11 },
        { "annotation-count", required_argument, NULL, 12 },
        { "annotation-freq-low", required_argument, NULL, 13 },
        { "annotation-freq-high", required_argument, NULL, 14 },
        { "annotation-add", no_argument, NULL, 15 },
        { "annotate-ts-jumps", no_argument, NULL, 16 },
        { "ts-jump-threshold-pct", required_argument, NULL, 17 },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);
    memset(&sigmf_meta, 0, sizeof(sigmf_meta));
    memset(&pending_annotation, 0, sizeof(pending_annotation));
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
            snprintf(pending_annotation.label, sizeof(pending_annotation.label), "%s", optarg);
            break;
        case 10:
            snprintf(pending_annotation.comment, sizeof(pending_annotation.comment), "%s", optarg);
            break;
        case 11:
            pending_annotation.sample_start = strtoull(optarg, NULL, 0);
            break;
        case 12:
            pending_annotation.sample_count = strtoull(optarg, NULL, 0);
            pending_annotation.has_sample_count = true;
            break;
        case 13:
            pending_annotation.freq_lower_edge = atof(optarg);
            pending_annotation.has_freq_lower_edge = true;
            break;
        case 14:
            pending_annotation.freq_upper_edge = atof(optarg);
            pending_annotation.has_freq_upper_edge = true;
            break;
        case 15:
            if (sigmf_finalize_pending_annotation(&sigmf_meta, &pending_annotation) != 0) {
                fprintf(stderr, "Could not append SigMF annotation\n");
                return 1;
            }
            break;
        case 16:
            annotate_ts_jumps = 1;
            break;
        case 17:
            ts_jump_threshold_pct = atof(optarg);
            break;
        default:
            exit(1);
        }
    }

    if (sigmf_finalize_pending_annotation(&sigmf_meta, &pending_annotation) != 0) {
        fprintf(stderr, "Could not append SigMF annotation\n");
        return 1;
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
        if (annotate_ts_jumps && !header) {
            fprintf(stderr, "--annotate-ts-jumps requires --enable-header\n");
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
                 sigmf_enable ? true : false, annotate_ts_jumps ? true : false, ts_jump_threshold_pct, &sigmf_meta);
    return 0;
}
