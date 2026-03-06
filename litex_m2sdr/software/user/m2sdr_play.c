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
           "  -h           Show this help message.\n"
#ifdef USE_LITEPCIE
           "  -c N         Use /dev/m2sdrN (default: 0).\n"
#elif defined(USE_LITEETH)
           "  -i IP        Target IP address (default: 192.168.1.50).\n"
           "  -p PORT      Target port (default: 1234).\n"
#endif
           "  -z           Zero-copy (accepted, ignored in sync API).\n"
           "  -8           Use 8-bit samples (SC8).\n"
           "  -q           Quiet mode.\n"
           "  -t           Timed start (align to next second).\n"
           "\n"
           "Arguments:\n"
           "  filename     Raw SC16 IQ file, or '-' for stdin.\n"
           "  loops        Number of times to loop playback (default=1, 0 for infinite).\n");
    exit(1);
}

static void m2sdr_play(const char *device_id, const char *filename, uint32_t loops, uint8_t quiet, uint8_t timed_start, enum m2sdr_format format)
{
    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }

    unsigned sample_size = m2sdr_format_size(format);
    unsigned samples_per_buf = DMA_BUFFER_SIZE / sample_size;
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
        close_fi = 1;
    }

    int i = 0;
    uint32_t current_loop = 0;
    int64_t last_time = get_time_ms();
    uint64_t total_buffers = 0;
    uint64_t last_buffers  = 0;
    uint64_t sw_underflows = 0;

    uint8_t buf[DMA_BUFFER_SIZE];
    while (keep_running) {
        size_t len = fread(buf, 1, DMA_BUFFER_SIZE, fi);
        if (ferror(fi)) {
            perror("fread");
            break;
        }
        if (feof(fi)) {
            current_loop++;
            if (loops != 0 && current_loop >= loops)
                break;
            if (strcmp(filename, "-") != 0) {
                fseek(fi, 0, SEEK_SET);
                clearerr(fi);
                len += fread(buf + len, 1, DMA_BUFFER_SIZE - len, fi);
            }
        }

        if (len < DMA_BUFFER_SIZE)
            memset(buf + len, 0, DMA_BUFFER_SIZE - len);

        if (m2sdr_sync_tx(dev, buf, samples_per_buf, NULL, 0) != 0) {
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
    #if defined(USE_LITEPCIE)
    static int m2sdr_device_num = 0;
    #endif
    static uint8_t quiet = 0;
    static uint8_t timed_start = 0;
    static enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;

    signal(SIGINT, intHandler);

    for (;;) {
#if defined(USE_LITEPCIE)
        c = getopt(argc, argv, "hc:zqt8");
#elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:zqt8");
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
        case '8':
            format = M2SDR_FORMAT_SC8_Q7;
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

    char device_id[128];
#ifdef USE_LITEPCIE
    snprintf(device_id, sizeof(device_id), "pcie:/dev/m2sdr%d", m2sdr_device_num);
#elif defined(USE_LITEETH)
    size_t ip_len = strnlen(m2sdr_ip_address, 256);
    size_t port_len = strnlen(m2sdr_port, sizeof(m2sdr_port));
    if (ip_len + port_len + sizeof("eth::") > sizeof(device_id)) {
        fprintf(stderr, "Device address too long\n");
        return 1;
    }
    snprintf(device_id, sizeof(device_id), "eth:%s:%s", m2sdr_ip_address, m2sdr_port);
#endif

    m2sdr_play(device_id, filename, loops, quiet, timed_start, format);
    return 0;
}
