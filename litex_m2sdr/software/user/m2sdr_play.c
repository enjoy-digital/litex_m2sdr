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
#include <stdbool.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_sigmf.h"
#include "config.h"

#include "liblitepcie.h"

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

static int read_next_play_frame(FILE *fi, int close_fi, uint32_t *current_loop, uint32_t loops,
                                uint64_t start_offset_bytes, uint64_t end_offset_bytes,
                                uint8_t *raw_buf, size_t frame_bytes)
{
    uint64_t current_pos;
    size_t to_read = frame_bytes;
    size_t len;

    if (close_fi && end_offset_bytes > 0) {
        current_pos = (uint64_t)ftello(fi);
        if (current_pos >= end_offset_bytes) {
            (*current_loop)++;
            if (loops != 0 && *current_loop >= loops)
                return 1;
            if (fseeko(fi, (off_t)start_offset_bytes, SEEK_SET) != 0) {
                perror("fseeko");
                return -1;
            }
            current_pos = start_offset_bytes;
        }
        if (current_pos + to_read > end_offset_bytes)
            to_read = (size_t)(end_offset_bytes - current_pos);
    }

    len = fread(raw_buf, 1, to_read, fi);
    if (ferror(fi)) {
        perror("fread");
        return -1;
    }
    if (feof(fi)) {
        (*current_loop)++;
        if (loops != 0 && *current_loop >= loops)
            return 1;
        if (close_fi) {
            if (fseeko(fi, (off_t)start_offset_bytes, SEEK_SET) != 0) {
                perror("fseeko");
                return -1;
            }
            clearerr(fi);
            len += fread(raw_buf + len, 1, frame_bytes - len, fi);
            if (ferror(fi)) {
                perror("fread");
                return -1;
            }
        }
    }

    if (len < frame_bytes)
        memset(raw_buf + len, 0, frame_bytes - len);

    return 0;
}

#ifdef USE_LITEPCIE
static void write_m2sdr_dma_header(uint8_t *buf, uint64_t timestamp)
{
    const uint64_t sync_word = M2SDR_DMA_HEADER_SYNC_WORD;

    memcpy(buf, &sync_word, sizeof(sync_word));
    memcpy(buf + 8, &timestamp, sizeof(timestamp));
}

static void fill_play_frame(uint8_t *dst, const uint8_t *src, size_t frame_bytes,
                            unsigned header_bytes, struct m2sdr_metadata *meta)
{
    memset(dst, 0, frame_bytes);

    if (header_bytes > 0) {
        uint64_t timestamp = 0;

        if (parse_m2sdr_dma_header(src, &timestamp)) {
            meta->timestamp = timestamp;
            meta->flags |= M2SDR_META_FLAG_HAS_TIME;
        }
        memcpy(dst + header_bytes, src + header_bytes, frame_bytes - header_bytes);
        write_m2sdr_dma_header(dst, (meta->flags & M2SDR_META_FLAG_HAS_TIME) ? meta->timestamp : 0);
    } else {
        memcpy(dst, src, frame_bytes);
    }
}
#endif

static void help(void)
{
    puts("M2SDR I/Q Player Utility.");
    puts("usage: m2sdr_play [options] <filename|-> [loops]");
    puts("");
    puts("Arguments:");
    puts("  filename     Raw IQ file, SigMF file/basename, or '-' for stdin.");
    puts("  loops        Number of times to loop playback (default: 1, 0 for infinite).");
    puts("");
    m2sdr_cli_print_device_help();
    puts("");
    puts("Playback options:");
    puts("  -h, --help            Show this help message.");
    puts("  -q, --quiet           Quiet mode.");
    puts("  -t, --timed-start     Timed start (align to the next second).");
    puts("      --format FMT      Sample format: sc16 or sc8 (default: sc16).");
    puts("");
    puts("SigMF input:");
    puts("      --capture-index N Select capture index from SigMF metadata (default: 0).");
    puts("");
    puts("Compatibility:");
    puts("      --zero-copy       Legacy compatibility flag; ignored in sync API.");
    puts("      --8bit            Legacy alias for --format sc8.");
    exit(1);
}

static void m2sdr_play(const char *device_id, const char *filename, uint32_t loops, uint8_t quiet,
                       uint8_t timed_start, enum m2sdr_format format, unsigned header_bytes,
                       uint64_t start_offset_bytes, uint64_t end_offset_bytes)
{
    struct m2sdr_dev *dev = NULL;
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;
#ifdef USE_LITEPCIE
    struct litepcie_dma_ctrl dma = {0};
    bool use_pcie_dma = false;
    int64_t hw_count_stop = 0;
#endif
    FILE *fi = NULL;
    int close_fi = 0;
    unsigned sample_size;
    unsigned samples_per_buf;
    size_t frame_bytes;
    int i = 0;
    uint32_t current_loop = 0;
    int64_t last_time;
    uint64_t total_buffers = 0;
    uint64_t last_buffers  = 0;
    uint64_t sw_underflows = 0;
    int exit_status = 1;
    uint8_t raw_buf[DMA_BUFFER_SIZE];
    uint8_t payload_buf[DMA_BUFFER_SIZE];

    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }
    if (m2sdr_get_transport(dev, &transport) != 0) {
        fprintf(stderr, "m2sdr_get_transport failed\n");
        goto cleanup;
    }

    sample_size = m2sdr_format_size(format);
    samples_per_buf = (DMA_BUFFER_SIZE - header_bytes) / sample_size;
    frame_bytes = (size_t)samples_per_buf * sample_size + header_bytes;

    if (m2sdr_set_tx_header(dev, header_bytes > 0) != 0) {
        fprintf(stderr, "m2sdr_set_tx_header failed\n");
        goto cleanup;
    }

#ifdef USE_LITEPCIE
    if (transport == M2SDR_TRANSPORT_KIND_LITEPCIE) {
        int fd = m2sdr_get_fd(dev);

        if (fd < 0) {
            fprintf(stderr, "m2sdr_get_fd failed\n");
            goto cleanup;
        }
        if (m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0) != 0) {
            fprintf(stderr, "CROSSBAR TX select failed\n");
            goto cleanup;
        }

        dma.use_reader = 1;
        dma.shared_fd  = 1;
        dma.fds.fd     = fd;
        if (litepcie_dma_init(&dma, NULL, 0) != 0) {
            fprintf(stderr, "litepcie_dma_init failed\n");
            goto cleanup;
        }
        dma.reader_enable = 1;
        use_pcie_dma = true;
    } else
#endif
    {
        if (m2sdr_sync_config(dev, M2SDR_TX, format,
                              0, samples_per_buf, 0, 1000) != 0) {
            fprintf(stderr, "m2sdr_sync_config failed\n");
            goto cleanup;
        }
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

    if (strcmp(filename, "-") == 0) {
        fi = stdin;
    } else {
        fi = fopen(filename, "rb");
        if (!fi) {
            perror(filename);
            goto cleanup;
        }
        if (start_offset_bytes > 0 && fseeko(fi, (off_t)start_offset_bytes, SEEK_SET) != 0) {
            perror("fseeko");
            goto cleanup;
        }
        close_fi = 1;
    }

    last_time = get_time_ms();
    while (1) {
#ifdef USE_LITEPCIE
        if (use_pcie_dma) {
            litepcie_dma_process(&dma);

            if (!keep_running) {
                hw_count_stop = dma.reader_sw_count + 16;
                break;
            }

            while (keep_running) {
                int rc;
                struct m2sdr_metadata meta = {0};
                char *dma_buf = litepcie_dma_next_write_buffer(&dma);

                if (!dma_buf)
                    break;
                if (dma.reader_sw_count - dma.reader_hw_count < 0)
                    sw_underflows += (uint64_t)(dma.reader_hw_count - dma.reader_sw_count);

                rc = read_next_play_frame(fi, close_fi, &current_loop, loops,
                                          start_offset_bytes, end_offset_bytes,
                                          raw_buf, frame_bytes);
                if (rc < 0)
                    goto cleanup;
                if (rc > 0) {
                    keep_running = 0;
                    break;
                }

                fill_play_frame((uint8_t *)dma_buf, raw_buf, frame_bytes, header_bytes, &meta);
            }
            total_buffers = (uint64_t)dma.reader_sw_count;
        } else
#endif
        {
            int rc;
            struct m2sdr_metadata meta = {0};

            if (!keep_running)
                break;

            rc = read_next_play_frame(fi, close_fi, &current_loop, loops,
                                      start_offset_bytes, end_offset_bytes,
                                      raw_buf, frame_bytes);
            if (rc < 0)
                goto cleanup;
            if (rc > 0)
                break;

            if (header_bytes > 0) {
                if (parse_m2sdr_dma_header(raw_buf, &meta.timestamp))
                    meta.flags |= M2SDR_META_FLAG_HAS_TIME;
                memcpy(payload_buf, raw_buf + header_bytes, frame_bytes - header_bytes);
            } else {
                memcpy(payload_buf, raw_buf, frame_bytes);
            }

            if (m2sdr_sync_tx(dev, header_bytes > 0 ? payload_buf : raw_buf, samples_per_buf,
                              header_bytes > 0 ? &meta : NULL, 0) != 0) {
                fprintf(stderr, "m2sdr_sync_tx failed\n");
                goto cleanup;
            }
            total_buffers++;
        }

        {
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
    }

#ifdef USE_LITEPCIE
    if (use_pcie_dma) {
        while (dma.reader_hw_count < hw_count_stop) {
            dma.reader_enable = 1;
            litepcie_dma_process(&dma);
        }
    }
#endif

    exit_status = 0;

cleanup:
#ifdef USE_LITEPCIE
    if (use_pcie_dma)
        litepcie_dma_cleanup(&dma);
#endif
    if (fi && close_fi)
        fclose(fi);
    if (dev)
        m2sdr_close(dev);
    if (exit_status != 0)
        exit(1);
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
            if (capture->header_bytes == M2SDR_HEADER_BYTES) {
                sigmf_header_bytes = capture->header_bytes;
            } else if (capture->header_bytes != 0) {
                fprintf(stderr, "SigMF playback does not currently support header_bytes=%u datasets\n",
                        capture->header_bytes);
                return 1;
            }
        } else if (sigmf_meta.has_header_bytes) {
            if (sigmf_meta.header_bytes == M2SDR_HEADER_BYTES) {
                sigmf_header_bytes = sigmf_meta.header_bytes;
            } else if (sigmf_meta.header_bytes != 0) {
                fprintf(stderr, "SigMF playback does not currently support header_bytes=%u datasets\n",
                        sigmf_meta.header_bytes);
                return 1;
            }
        }

        format = sigmf_format;
        if (capture && sigmf_header_bytes == 0) {
            if (m2sdr_sigmf_capture_byte_range(&sigmf_meta, capture_index, format, 0, DMA_BUFFER_SIZE,
                                               &start_offset_bytes, &end_offset_bytes) != 0) {
                fprintf(stderr, "Could not derive capture byte range from SigMF metadata\n");
                return 1;
            }
        } else if (capture && sigmf_header_bytes != 0) {
            if (m2sdr_sigmf_capture_byte_range(&sigmf_meta, capture_index, format, sigmf_header_bytes, DMA_BUFFER_SIZE,
                                               &start_offset_bytes, &end_offset_bytes) != 0) {
                fprintf(stderr, "Capture-local looping requires frame-aligned headered SigMF captures; replaying full file\n");
            }
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
