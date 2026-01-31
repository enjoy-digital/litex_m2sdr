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
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <time.h>

#include "liblitepcie.h"

#include "libm2sdr.h"

#if defined(USE_LITEETH)
#include "etherbone.h"
#include "liteeth_udp.h"
#endif

/* Variables */
/*-----------*/

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

/* forward (provided in tree elsewhere; used here) */
extern int64_t get_time_ms(void);

#if defined(USE_LITEETH)
/* Ethernet configuration (mirrors m2sdr_rf defaults & flags) */
static char m2sdr_ip_address[1024] = "192.168.1.50";
static char m2sdr_port[16] = "1234";
#endif

/* Record (DMA RX / UDP RX) */
/*--------------------------*/

#if defined(USE_LITEPCIE)

static void m2sdr_record(const char *device_name, const char *filename, size_t size, uint8_t zero_copy, uint8_t quiet, uint8_t header, uint8_t strip_header)
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

    /* Crossbar Demux: select PCIe streaming for RX */
    m2sdr_writel(dma.fds.fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

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
                size_t data_off = 0;
                if (header && strip_header && DMA_BUFFER_SIZE >= 16) {
                    data_off = 16;
                    to_write -= 16;
                }
                if (size > 0 && to_write > size - total_len) {
                    to_write = size - total_len;
                }
                len = fwrite(buf_rd + data_off, 1, to_write, fo);
                if (len != to_write) {
                    perror("fwrite");
                    keep_running = 0;
                    break;
                }
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

#elif defined(USE_LITEETH)

/* UDP-based RX path with CLI-configurable IP/port (mirrors m2sdr_rf). */
static void m2sdr_record(const char *device_name, const char *filename, size_t size, uint8_t zero_copy, uint8_t quiet, uint8_t header, uint8_t strip_header)
{
    (void)device_name; (void)zero_copy;

    static const uint64_t HDR_SYNC_WORD = 0x5aa55aa55aa55aa5ULL;

    /* Use CLI-provided IP/port */
    const char *ip   = m2sdr_ip_address;
    const char *port = m2sdr_port;
    uint16_t udp_port = (uint16_t)strtoul(port, NULL, 0);

    /* Etherbone for CSR access (header enable) */
    struct eb_connection *eb = eb_connect(ip, port, 1);
    if (!eb) {
        fprintf(stderr, "Failed to connect to %s:%s\n", ip, port);
        exit(1);
    }

    /* Configure RX Header (match PCIe path behavior). */
    m2sdr_writel(eb, CSR_HEADER_RX_CONTROL_ADDR,
       (1      << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
       (header << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)
    );

    /* Crossbar Demux: select Ethernet streaming for RX */
    m2sdr_writel(eb, CSR_CROSSBAR_DEMUX_SEL_ADDR, 1);

    /* UDP helper (RX only). Buffer sizing: library defaults. */
    struct liteeth_udp_ctrl udp;
    if (liteeth_udp_init(&udp,
                         /*listen_ip*/ NULL, /*listen_port*/ 2345,
                         /*remote_ip*/ ip,   /*remote_port*/ udp_port,
                         /*rx_enable*/ 1,    /*tx_enable*/ 0,
                         /*buffer_size*/ 0,  /*buffer_count*/ 0,
                         /*nonblock*/   0) < 0) {
        fprintf(stderr, "liteeth_udp_init failed\n");
        eb_disconnect(&eb);
        exit(1);
    }
    /* enlarge SO_RCVBUF a bit for bursts */
    liteeth_udp_set_so_rcvbuf(&udp, 4*1024*1024);

    /* File / stdout */
    FILE *fo = NULL;
    if (filename != NULL) {
        if (strcmp(filename, "-") == 0) {
            fo = stdout;
        } else {
            fo = fopen(filename, "wb");
            if (!fo) {
                perror(filename);
                liteeth_udp_cleanup(&udp);
                eb_disconnect(&eb);
                exit(1);
            }
        }
    }

    /* Stats & loop */
    int i = 0;
    size_t total_len = 0;
    uint64_t last_timestamp = 0;
    int64_t last_time = get_time_ms();
    uint64_t total_buffers  = 0;
    uint64_t last_buffers   = 0;

    for (;;) {
        if (!keep_running)
            break;

        /* Pump socket; assemble into ring. */
        liteeth_udp_process(&udp, 100);

        /* Drain available buffers. */
        while (1) {
            uint8_t *buf = liteeth_udp_next_read_buffer(&udp);
            if (!buf) break;

            if (header) {
                uint64_t sync = *(uint64_t*)buf;
                if (sync == HDR_SYNC_WORD) {
                    last_timestamp = *(uint64_t*)(buf + 8);
                }
            }

            if (fo != NULL) {
                if (size > 0 && total_len >= size) {
                    keep_running = 0;
                    break;
                }
                size_t to_write = udp.buf_size;
                size_t data_off = 0;
                if (header && strip_header && udp.buf_size >= 16) {
                    data_off = 16;
                    to_write -= 16;
                }
                if (size > 0 && to_write > size - total_len)
                    to_write = size - total_len;

                size_t n = fwrite(buf + data_off, 1, to_write, fo);
                if (n != to_write) {
                    perror("fwrite");
                    keep_running = 0;
                    break;
                }
                total_len += n;
            }

            total_buffers++;
        }

        /* Statistics every ~200ms. */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            uint64_t delta_buffers = total_buffers - last_buffers;
            double   speed_gbps    = (double)delta_buffers * udp.buf_size * 8 / ((double)duration * 1e6);
            uint64_t size_mb  = (total_buffers * udp.buf_size) / 1024 / 1024;

            if (i % 10 == 0) {
                if (header) {
                    fprintf(stderr, "\e[1m%10s %10s %8s %23s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)", "TIME");
                } else {
                    fprintf(stderr, "\e[1m%10s %10s %9s\e[0m\n", "SPEED(Gbps)", "BUFFERS", "SIZE(MB)");
                }
            }
            i++;

            if (header) {
                uint64_t seconds = last_timestamp / 1000000000ULL;
                uint64_t nanos   = last_timestamp % 1000000000ULL;
                uint32_t ms      = nanos / 1000000;
                struct tm tm;
                gmtime_r((const time_t*)&seconds, &tm);
                char time_str[64];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
                fprintf(stderr, "%11.2f %10" PRIu64 " %8" PRIu64 " %19s.%03u\n",
                        speed_gbps, total_buffers, size_mb, time_str, ms);
            } else {
                fprintf(stderr, "%11.2f %10" PRIu64 " %8" PRIu64 "\n",
                        speed_gbps, total_buffers, size_mb);
            }

            /* reset interval */
            last_time = get_time_ms();
            last_buffers = total_buffers;
        }
    }

    liteeth_udp_cleanup(&udp);
    eb_disconnect(&eb);

    if (fo != NULL && fo != stdout)
        fclose(fo);
}

#else
#error "Define USE_LITEPCIE or USE_LITEETH for build configuration"
#endif

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR I/Q Record Utility\n"
           "usage: m2sdr_record [options] [filename] [size]\n"
           "\n"
           "Options:\n"
           "-h                    Display this help message.\n"
#if defined(USE_LITEPCIE)
           "-c device_num         Select the device (default = 0).\n"
#elif defined(USE_LITEETH)
           "-i ip_address         Target IP address for Etherbone/UDP (default: 192.168.1.50).\n"
           "-p port               Port number (default = 1234).\n"
#endif
           "-z                    Enable zero-copy DMA mode.\n"
           "-q                    Quiet mode (suppress statistics).\n"
           "-t                    Enable RX Header with timestamp.\n"
           "-s                    Strip 16-byte RX header from output.\n"
           "\n"
           "Arguments:\n"
           "filename              File to record I/Q samples to (optional, omit to monitor stream).\n"
           "size                  Number of bytes to record (optional, 0 for infinite).\n");
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
    static uint8_t strip_header = 0;
    m2sdr_device_num       = 0;
    m2sdr_device_zero_copy = 0;

    signal(SIGINT, intHandler);

    /* Parameters. */
    for (;;) {
#if defined(USE_LITEPCIE)
        c = getopt(argc, argv, "hc:zqts");
#elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:zqts");
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
            m2sdr_device_zero_copy = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        case 't':
            header = 1;
            break;
        case 's':
            strip_header = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
#if defined(USE_LITEPCIE)
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);
#endif

    /* Interpret cmd and record. */
    const char *filename = NULL;
    size_t size = 0;
    if (optind != argc) {
        filename = argv[optind++];  /* Allow filename to be provided or "-" */
        if (optind < argc) {        /* Size is optional */
            size = strtoul(argv[optind++], NULL, 0);
        }
    }
    m2sdr_record(m2sdr_device, filename, size, m2sdr_device_zero_copy, quiet, header, strip_header);
    return 0;
}
