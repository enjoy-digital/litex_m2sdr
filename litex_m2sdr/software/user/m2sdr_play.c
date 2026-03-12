/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR I/Q Player Utility.
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
#include "m2sdr_sigmf.h"
#include "config.h"

#include "litepcie_helpers.h"

#define M2SDR_DMA_HEADER_SIZE 16
#define M2SDR_DMA_HEADER_SYNC_WORD 0x5aa55aa55aa55aa5ULL

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

static int parse_m2sdr_dma_header(const uint8_t *buf, uint64_t *timestamp)
{
    uint64_t sync_word = 0;
    uint64_t ts = 0;

    memcpy(&sync_word, buf, sizeof(sync_word));
    if (sync_word != M2SDR_DMA_HEADER_SYNC_WORD)
        return 0;
    memcpy(&ts, buf + 8, sizeof(ts));
    *timestamp = ts;
    return 1;
}

static void print_time_banner(const char *label, uint64_t time_ns)
{
    time_t seconds = (time_t)(time_ns / 1000000000ULL);
    uint32_t ms = (time_ns % 1000000000ULL) / 1000000;
    struct tm tm;
    char time_str[64];

    localtime_r(&seconds, &tm);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
    printf("%s %s.%03u\n", label, time_str, ms);
}

static void help(void)
{
    printf("M2SDR I/Q Player Utility.\n"
           "usage: m2sdr_play [options] <filename|- > [loops]\n"
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
           "  -t, --timed-start     Timed start (align to next second).\n"
           "      --capture-index N SigMF capture index to use (default: 0).\n"
           "      --format FMT      Sample format: sc16 or sc8 (default: sc16).\n"
           "      --zero-copy       Legacy compatibility flag; ignored in sync API.\n"
           "      --8bit            Legacy alias for --format sc8.\n"
           "\n"
           "Arguments:\n"
           "  filename     Raw IQ file, SigMF file/basename, or '-' for stdin.\n"
           "  loops        Number of times to loop playback (default=1, 0 for infinite).\n");
    exit(1);
}

static void m2sdr_play(const char *device_id, const char *filename, uint32_t loops, uint8_t quiet,
                       uint8_t timed_start, enum m2sdr_format format, unsigned header_bytes,
                       uint64_t start_offset_bytes, uint64_t end_offset_bytes)
{
    struct m2sdr_dev *dev = NULL;
    size_t frame_bytes;
    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }

    unsigned sample_size = m2sdr_format_size(format);
    unsigned samples_per_buf = (DMA_BUFFER_SIZE - header_bytes) / sample_size;
    frame_bytes = (size_t)samples_per_buf * sample_size + header_bytes;
    if (header_bytes > 0) {
        if (m2sdr_set_tx_header(dev, true) != 0) {
            fprintf(stderr, "m2sdr_set_tx_header failed\n");
            m2sdr_close(dev);
            exit(1);
        }
    }
    if (m2sdr_sync_config(dev, M2SDR_TX, format,
                          0, samples_per_buf, 0, 1000) != 0) {
        fprintf(stderr, "m2sdr_sync_config failed\n");
        m2sdr_close(dev);
        exit(1);
    }

    if (timed_start) {
        uint64_t current_ts = 0;
        if (m2sdr_get_time(dev, &current_ts) == 0) {
            print_time_banner("Initial Time :", current_ts);
            uint64_t ns_in_sec = current_ts % 1000000000ULL;
            uint64_t wait_ns   = (ns_in_sec == 0) ? 1000000000ULL : (1000000000ULL - ns_in_sec);
            uint64_t target_ts = current_ts + wait_ns;
            while (current_ts < target_ts) {
                m2sdr_get_time(dev, &current_ts);
                usleep(1000);
            }
            usleep(100000);
            print_time_banner("Start Time   :", target_ts + 1000000000ULL);
        }
    }

    FILE *fi;
    int close_fi = 0;
    if (strcmp(filename, "-") == 0) {
        fi = stdin;
    } else {
        fi = fopen(filename, "rb");
        if (!fi) {
            perror(filename);
            m2sdr_close(dev);
            exit(1);
        }
        if (start_offset_bytes > 0 && fseeko(fi, (off_t)start_offset_bytes, SEEK_SET) != 0) {
            perror("fseeko");
            fclose(fi);
            m2sdr_close(dev);
            exit(1);
        }
        close_fi = 1;
    }

    int i = 0;
    uint32_t current_loop = 0;
    int64_t last_time = get_time_ms();
    uint64_t total_buffers = 0;
    uint64_t last_buffers  = 0;
    uint64_t sw_underflows = 0;

    uint8_t raw_buf[DMA_BUFFER_SIZE];
    uint8_t payload_buf[DMA_BUFFER_SIZE];
    while (keep_running) {
        uint64_t current_pos;
        size_t to_read = frame_bytes;
        size_t len;
        struct m2sdr_metadata meta = {0};

        if (close_fi && end_offset_bytes > 0) {
            current_pos = (uint64_t)ftello(fi);
            if (current_pos >= end_offset_bytes) {
                current_loop++;
                if (loops != 0 && current_loop >= loops)
                    break;
                if (fseeko(fi, (off_t)start_offset_bytes, SEEK_SET) != 0) {
                    perror("fseeko");
                    break;
                }
                current_pos = start_offset_bytes;
            }
            if (current_pos + to_read > end_offset_bytes)
                to_read = (size_t)(end_offset_bytes - current_pos);
        }

        len = fread(raw_buf, 1, to_read, fi);
        if (ferror(fi)) {
            perror("fread");
            break;
        }
        if (feof(fi)) {
            current_loop++;
            if (loops != 0 && current_loop >= loops)
                break;
            if (strcmp(filename, "-") != 0) {
                fseeko(fi, (off_t)start_offset_bytes, SEEK_SET);
                clearerr(fi);
                len += fread(raw_buf + len, 1, frame_bytes - len, fi);
            }
        }

        if (len < frame_bytes)
            memset(raw_buf + len, 0, frame_bytes - len);

        if (header_bytes > 0) {
            if (len >= M2SDR_DMA_HEADER_SIZE && parse_m2sdr_dma_header(raw_buf, &meta.timestamp))
                meta.flags |= M2SDR_META_FLAG_HAS_TIME;
            memcpy(payload_buf, raw_buf + header_bytes, frame_bytes - header_bytes);
        } else {
            memcpy(payload_buf, raw_buf, frame_bytes);
        }

        if (m2sdr_sync_tx(dev, header_bytes > 0 ? payload_buf : raw_buf, samples_per_buf,
                          header_bytes > 0 ? &meta : NULL, 0) != 0) {
            fprintf(stderr, "m2sdr_sync_tx failed\n");
            break;
        }
        total_buffers++;

        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            double speed  = (double)(total_buffers - last_buffers) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6);
            uint64_t size = (total_buffers * DMA_BUFFER_SIZE) / 1024 / 1024;

            if (i % 10 == 0)
                fprintf(stderr, "\e[1mSPEED(Gbps)   BUFFERS   SIZE(MB)   LOOP UNDERFLOWS\e[0m\n");
            i++;

            fprintf(stderr, "%10.2f %10" PRIu64 " %10" PRIu64 " %6u %10" PRIu64 "\n",
                    speed, total_buffers, size, current_loop, sw_underflows);

            last_time = get_time_ms();
            last_buffers = total_buffers;
            sw_underflows = 0;
        }
    }

    if (close_fi)
        fclose(fi);
    m2sdr_close(dev);
}

int main(int argc, char **argv)
{
    int c;
    int option_index = 0;
    static uint8_t quiet = 0;
    static uint8_t timed_start = 0;
    static enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    unsigned capture_index = 0;
    unsigned sigmf_header_bytes = 0;
    uint64_t start_offset_bytes = 0;
    uint64_t end_offset_bytes = 0;
    struct m2sdr_sigmf_meta sigmf_meta;
    char resolved_filename[1024] = {0};
    struct m2sdr_cli_device cli_dev;
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "quiet", no_argument, NULL, 'q' },
        { "timed-start", no_argument, NULL, 't' },
        { "zero-copy", no_argument, NULL, 'z' },
        { "capture-index", required_argument, NULL, 3 },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);
    memset(&sigmf_meta, 0, sizeof(sigmf_meta));

    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:p:zqt", options, &option_index);
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
            capture_index = (unsigned)strtoul(optarg, NULL, 0);
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

    if (strcmp(filename, "-") != 0 && m2sdr_sigmf_read(filename, &sigmf_meta) == 0) {
        enum m2sdr_format sigmf_format = m2sdr_sigmf_format_from_datatype(sigmf_meta.datatype);
        const struct m2sdr_sigmf_capture *capture = NULL;

        if (sigmf_format == (enum m2sdr_format)-1) {
            fprintf(stderr, "Unsupported SigMF datatype: %s\n", sigmf_meta.datatype);
            return 1;
        }
        if (sigmf_meta.capture_count > 0) {
            if (capture_index >= sigmf_meta.capture_count) {
                fprintf(stderr, "SigMF capture index %u out of range (captures=%u)\n",
                        capture_index, sigmf_meta.capture_count);
                return 1;
            }
            capture = &sigmf_meta.captures[capture_index];
        }
        if (capture && capture->has_header_bytes) {
            if (capture->header_bytes == M2SDR_DMA_HEADER_SIZE) {
                sigmf_header_bytes = capture->header_bytes;
            } else if (capture->header_bytes != 0) {
                fprintf(stderr, "SigMF playback does not currently support header_bytes=%u datasets\n",
                        capture->header_bytes);
                return 1;
            }
        } else if (sigmf_meta.has_header_bytes) {
            if (sigmf_meta.header_bytes == M2SDR_DMA_HEADER_SIZE) {
                sigmf_header_bytes = sigmf_meta.header_bytes;
            } else if (sigmf_meta.header_bytes != 0) {
                fprintf(stderr, "SigMF playback does not currently support header_bytes=%u datasets\n",
                        sigmf_meta.header_bytes);
                return 1;
            }
        }

        format = sigmf_format;
        if (capture && sigmf_header_bytes == 0) {
            uint64_t start_sample = 0;
            uint64_t end_sample = 0;
            size_t sample_size = m2sdr_format_size(format);

            if (sample_size == 0 ||
                m2sdr_sigmf_capture_sample_range(&sigmf_meta, capture_index, &start_sample, &end_sample) != 0) {
                fprintf(stderr, "Could not derive capture sample range from SigMF metadata\n");
                return 1;
            }
            start_offset_bytes = start_sample * (uint64_t)sample_size;
            if (end_sample > start_sample)
                end_offset_bytes = end_sample * (uint64_t)sample_size;
        } else if (capture && sigmf_header_bytes != 0) {
            fprintf(stderr, "Capture-local looping is not available for headered SigMF datasets; replaying full file\n");
        }
        snprintf(resolved_filename, sizeof(resolved_filename), "%s", sigmf_meta.data_path);
        filename = resolved_filename;
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        return 1;

    m2sdr_play(m2sdr_cli_device_id(&cli_dev), filename, loops, quiet, timed_start, format,
               sigmf_header_bytes, start_offset_bytes, end_offset_bytes);
    return 0;
}
