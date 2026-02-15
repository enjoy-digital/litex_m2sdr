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
/* Ethernet configuration (align with m2sdr_record defaults & flags) */
static char m2sdr_ip_address[1024] = "192.168.1.50";
static char m2sdr_port[16] = "1234";
#endif

/* Play (DMA TX / UDP TX) */
/*------------------------*/

#if defined(USE_LITEPCIE)

static void m2sdr_play(const char *device_name, const char *filename, uint32_t loops, uint8_t zero_copy, uint8_t quiet, uint8_t timed_start)
{
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    FILE *fi;
    int close_fi = 0;
    int i = 0;
    size_t len;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint32_t current_loop = 0;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;

    /* Open File or use stdin */
    if (strcmp(filename, "-") == 0) {
        fi = stdin;
    } else {
        fi = fopen(filename, "rb");
        if (!fi) {
            perror(filename);
            exit(1);
        }
        close_fi = 1;
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
        if (!keep_running) {
            hw_count_stop = dma.reader_sw_count + 16;
            break;
        }

        /* Write to DMA. */
        while (1) {
            /* Get Write buffer. */
            char *buf_wr = litepcie_dma_next_write_buffer(&dma);
            /* Break when no buffer available for Write */
            if (!buf_wr)
                break;

            /* Detect DMA underflows. */
            if (dma.reader_sw_count - dma.reader_hw_count < 0)
                sw_underflows += (dma.reader_hw_count - dma.reader_sw_count);

            /* Read data from File and fill Write buffer */
            len = fread(buf_wr, 1, DMA_BUFFER_SIZE, fi);
            if (ferror(fi)) {
                perror("fread");
                keep_running = 0;
            }
            if (feof(fi)) {
                /* Rewind on end of file, but not for stdin */
                current_loop += 1;
                if (loops != 0 && current_loop >= loops)
                    keep_running = 0;
                if (strcmp(filename, "-") != 0) {
                    rewind(fi);
                    len += fread(buf_wr + len, 1, DMA_BUFFER_SIZE - len, fi);
                }
            }
            if (len < DMA_BUFFER_SIZE) {
                memset(buf_wr + len, 0, DMA_BUFFER_SIZE - len);
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
            printf("%10.2f %10" PRIu64 " %10" PRIu64 " %6u %10ld\n",
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
    if (close_fi)
        fclose(fi);
}

#elif defined(USE_LITEETH)

/* UDP-based TX path with CLI-configurable IP/port (mirrors m2sdr_record). */
static void m2sdr_play(const char *device_name, const char *filename, uint32_t loops, uint8_t zero_copy, uint8_t quiet, uint8_t timed_start)
{
    (void)device_name; (void)zero_copy;

    /* Use CLI-provided IP/port */
    const char *ip   = m2sdr_ip_address;
    const char *port = m2sdr_port;
    uint16_t udp_port = (uint16_t)strtoul(port, NULL, 0);

    /* Etherbone for CSR access (crossbar + optional timed start). */
    struct eb_connection *eb = eb_connect(ip, port, 1);
    if (!eb) {
        fprintf(stderr, "Failed to connect to %s:%s\n", ip, port);
        exit(1);
    }

    /* Crossbar Mux: select Ethernet streaming for TX. */
    m2sdr_writel(eb, CSR_CROSSBAR_MUX_SEL_ADDR, 1);

    if (timed_start) {
        /* Read initial timestamp (same as PCIe path). */
        uint32_t ctrl = m2sdr_readl(eb, CSR_TIME_GEN_CONTROL_ADDR);
        m2sdr_writel(eb, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
        m2sdr_writel(eb, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);
        uint64_t current_ts = (
            ((uint64_t) m2sdr_readl(eb, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32 |
            ((uint64_t) m2sdr_readl(eb, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0
        );
        time_t seconds = current_ts / 1000000000ULL;
        uint32_t ms = (current_ts % 1000000000ULL) / 1000000;
        struct tm tm;
        localtime_r(&seconds, &tm);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
        printf("Initial Time : %s.%03u\n", time_str, ms);

        /* Align to next full second and add 100ms margin (like PCIe). */
        uint64_t ns_in_sec = current_ts % 1000000000ULL;
        uint64_t wait_ns   = (ns_in_sec == 0) ? 1000000000ULL : (1000000000ULL - ns_in_sec);
        uint64_t target_ts = current_ts + wait_ns;

        uint64_t poll_ts;
        do {
            ctrl = m2sdr_readl(eb, CSR_TIME_GEN_CONTROL_ADDR);
            m2sdr_writel(eb, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
            m2sdr_writel(eb, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);
            poll_ts = (
                ((uint64_t) m2sdr_readl(eb, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32 |
                ((uint64_t) m2sdr_readl(eb, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0
            );
            usleep(1000);
        } while (poll_ts < target_ts);
        usleep(100000);

        target_ts += 1000000000ULL;
        seconds = target_ts / 1000000000ULL;
        ms      = (target_ts % 1000000000ULL) / 1000000;
        localtime_r(&seconds, &tm);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
        printf("Start Time   : %s.%03u\n", time_str, ms);
    }

    /* UDP helper (TX only). Buffer sizing: library defaults. */
    struct liteeth_udp_ctrl udp;
    if (liteeth_udp_init(&udp,
                         /*listen_ip*/ NULL, /*listen_port*/ 0,
                         /*remote_ip*/ ip,   /*remote_port*/ udp_port,
                         /*rx_enable*/ 0,    /*tx_enable*/ 1,
                         /*buffer_size*/ 0,  /*buffer_count*/ 0,
                         /*nonblock*/   0) < 0) {
        fprintf(stderr, "liteeth_udp_init failed\n");
        eb_disconnect(&eb);
        exit(1);
    }
    /* grow send buffer for high throughput */
    liteeth_udp_set_so_sndbuf(&udp, 4*1024*1024);

    /* Open File or use stdin */
    FILE *fi;
    int close_fi = 0;
    if (strcmp(filename, "-") == 0) {
        fi = stdin;
    } else {
        fi = fopen(filename, "rb");
        if (!fi) {
            perror(filename);
            liteeth_udp_cleanup(&udp);
            eb_disconnect(&eb);
            exit(1);
        }
        close_fi = 1;
    }

    /* Stats & loop */
    int i = 0;
    uint32_t current_loop = 0;
    int64_t last_time = get_time_ms();
    uint64_t total_buffers = 0;
    uint64_t last_buffers  = 0;

    for (;;) {
        if (!keep_running)
            break;

        /* Fill and submit UDP frames while buffers are available. */
        while (1) {
            uint8_t *buf = liteeth_udp_next_write_buffer(&udp);
            if (!buf) break; /* should not happen with our simple ring */

            size_t copied = fread(buf, 1, udp.buf_size, fi);
            if (copied < udp.buf_size) {
                /* EOF: loop or finish (stdin never loops). */
                current_loop += 1;
                if (loops != 0 && current_loop >= loops) {
                    /* Send a final short frame if any bytes read. */
                    if (copied > 0) {
                        /* zero pad remaining to keep fixed-size frames to the FPGA if desired */
                        memset(buf + copied, 0, udp.buf_size - copied);
                        if (liteeth_udp_write_submit(&udp) < 0) {
                            keep_running = 0;
                            break;
                        }
                        total_buffers++;
                    }
                    keep_running = 0;
                    break;
                }
                if (strcmp(filename, "-") != 0) {
                    rewind(fi);
                    /* top-up the remainder of the frame after rewind */
                    size_t need = udp.buf_size - copied;
                    if (need) {
                        size_t extra = fread(buf + copied, 1, need, fi);
                        if (extra < need) {
                            /* file shorter than one frame: pad */
                            memset(buf + copied + extra, 0, need - extra);
                        }
                    }
                } else {
                    /* stdin: pad and continue */
                    memset(buf + copied, 0, udp.buf_size - copied);
                }
            }

            if (liteeth_udp_write_submit(&udp) < 0) {
                keep_running = 0;
                break;
            }
            total_buffers++;
        }

        if (!keep_running) break;

        /* Periodic stats (~200ms). */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            uint64_t delta_buffers = total_buffers - last_buffers;
            double speed_gbps = (double)delta_buffers * udp.buf_size * 8 / ((double)duration * 1e6);
            uint64_t size_mb  = (total_buffers * udp.buf_size) / 1024 / 1024;

            if (i % 10 == 0) {
                printf("\e[1m%10s %10s %9s %6s\e[0m\n",
                        "SPEED(Gbps)", "BUFFERS", "SIZE(MB)", "LOOP");
            }
            i++;

            printf("%11.2f %10" PRIu64 " %8" PRIu64 " %6u\n",
                    speed_gbps, total_buffers, size_mb, current_loop);

            last_time = get_time_ms();
            last_buffers = total_buffers;
        }
    }

    liteeth_udp_cleanup(&udp);
    eb_disconnect(&eb);

    if (close_fi)
        fclose(fi);
}

#else
#error "Define USE_LITEPCIE or USE_LITEETH for build configuration"
#endif

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR I/Q Player Utility\n"
           "usage: m2sdr_play [options] filename [loops]\n"
           "\n"
           "Options:\n"
           "-h                    Display this help message.\n"
#if defined(USE_LITEPCIE)
           "-c device_num         Select the device (default = 0).\n"
#elif defined(USE_LITEETH)
           "-i ip_address         Target IP address for Etherbone/UDP (default: 192.168.1.50).\n"
           "-p port               Port number (default = 1234).\n"
#endif
           "-z                    Enable zero-copy DMA mode (PCIe only).\n"
           "-q                    Quiet mode (suppress statistics).\n"
           "-t                    Timed start mode.\n"
           "\n"
           "Arguments:\n"
           "filename              I/Q samples stream file to play (use '-' for stdin).\n"
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
#if defined(USE_LITEPCIE)
        c = getopt(argc, argv, "hc:zqt");
#elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:zqt");
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
            timed_start = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device (PCIe). */
#if defined(USE_LITEPCIE)
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);
#endif

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
