/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA Utility.
 *
 * Configure Crossbar routing and drive LiteSATA streamers:
 * - record:  RX stream -> SATA_RX_STREAMER (Stream2Sectors)
 * - play:    SATA_TX_STREAMER -> TX stream (Sectors2Stream)
 * - replay:  SATA_TX_STREAMER -> TXRX loopback -> RX destination (pcie/eth/sata)
 * - copy:    SATA_TX_STREAMER -> TXRX loopback -> SATA_RX_STREAMER (SSD -> SSD)
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
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>

#include "liblitepcie.h"
#include "libm2sdr.h"
#include "csr.h"

/* Connection options -------------------------------------------------------- */

#ifdef USE_LITEPCIE
static char m2sdr_device[1024];
static int  m2sdr_device_num = 0;
#elif defined(USE_LITEETH)
static char m2sdr_ip_address[1024] = "192.168.1.50";
static char m2sdr_port[16]         = "1234";
#endif

/* Helpers ------------------------------------------------------------------- */

static uint64_t parse_u64(const char *s)
{
    char *end = NULL;
    uint64_t v = strtoull(s, &end, 0);
    if (!s || !*s || (end && *end)) {
        fprintf(stderr, "Invalid number: '%s'\n", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static uint32_t parse_u32(const char *s)
{
    uint64_t v = parse_u64(s);
    if (v > UINT32_MAX) {
        fprintf(stderr, "Value out of range for u32: %" PRIu64 "\n", v);
        exit(1);
    }
    return (uint32_t)v;
}

static __attribute__((unused)) void msleep(unsigned ms)
{
    usleep(ms * 1000);
}

/* Connection functions ------------------------------------------------------ */

static void *m2sdr_open(void)
{
#ifdef USE_LITEPCIE
    int fd = open(m2sdr_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s\n", m2sdr_device);
        exit(1);
    }
    return (void *)(intptr_t)fd;
#elif defined(USE_LITEETH)
    struct eb_connection *eb = eb_connect(m2sdr_ip_address, m2sdr_port, 1);
    if (!eb) {
        fprintf(stderr, "Failed to connect to %s:%s\n", m2sdr_ip_address, m2sdr_port);
        exit(1);
    }
    return eb;
#endif
}

static void m2sdr_close(void *conn)
{
#ifdef USE_LITEPCIE
    close((int)(intptr_t)conn);
#elif defined(USE_LITEETH)
    eb_disconnect((struct eb_connection **)&conn);
#endif
}

/* 64-bit CSR access (LiteX ordering: upper @ base+0, lower @ base+4) -------- */

static __attribute__((unused)) void csr_write64(void *conn, uint32_t addr, uint64_t v)
{
    m2sdr_writel(conn, addr + 0, (uint32_t)(v >> 32));
    m2sdr_writel(conn, addr + 4, (uint32_t)(v >>  0));
}

static  __attribute__((unused))  uint64_t csr_read64(void *conn, uint32_t addr)
{
    uint32_t upper = m2sdr_readl(conn, addr + 0);
    uint32_t lower = m2sdr_readl(conn, addr + 4);
    return ((uint64_t)upper << 32) | (uint64_t)lower;
}

/* Crossbar routing ---------------------------------------------------------- */

enum {
    TXSRC_PCIE = 0,
    TXSRC_ETH  = 1,
    TXSRC_SATA = 2,
};

enum {
    RXDST_PCIE = 0,
    RXDST_ETH  = 1,
    RXDST_SATA = 2,
};

static int parse_txsrc(const char *s)
{
    if (!strcmp(s, "pcie")) return TXSRC_PCIE;
    if (!strcmp(s, "eth"))  return TXSRC_ETH;
    if (!strcmp(s, "sata")) return TXSRC_SATA;
    fprintf(stderr, "Invalid tx source: %s (expected: pcie|eth|sata)\n", s);
    exit(1);
}

static int parse_rxdst(const char *s)
{
    if (!strcmp(s, "pcie")) return RXDST_PCIE;
    if (!strcmp(s, "eth"))  return RXDST_ETH;
    if (!strcmp(s, "sata")) return RXDST_SATA;
    fprintf(stderr, "Invalid rx destination: %s (expected: pcie|eth|sata)\n", s);
    exit(1);
}

static void crossbar_set(void *conn, int txsrc, int rxdst)
{
#ifdef CSR_CROSSBAR_BASE
    m2sdr_writel(conn, CSR_CROSSBAR_MUX_SEL_ADDR,   (uint32_t)txsrc);
    m2sdr_writel(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR, (uint32_t)rxdst);
#else
    (void)conn; (void)txsrc; (void)rxdst;
    fprintf(stderr, "Crossbar CSR not present in this gateware.\n");
    exit(1);
#endif
}

/* Optional header raw control ---------------------------------------------- */

static void header_set_raw(void *conn, int which, int enable, int header_enable)
{
#ifdef CSR_HEADER_BASE
    uint32_t v;

    v  = 0;
    v |= (enable        ? 1u : 0u) << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET;
    v |= (header_enable ? 1u : 0u) << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET;
    if (which == 0 || which == 2)
        m2sdr_writel(conn, CSR_HEADER_TX_CONTROL_ADDR, v);

    v  = 0;
    v |= (enable        ? 1u : 0u) << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET;
    v |= (header_enable ? 1u : 0u) << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET;
    if (which == 1 || which == 2)
        m2sdr_writel(conn, CSR_HEADER_RX_CONTROL_ADDR, v);
#else
    (void)conn; (void)which; (void)enable; (void)header_enable;
    fprintf(stderr, "Header CSR not present.\n");
    exit(1);
#endif
}

/* SATA control (guarded) ---------------------------------------------------- */

#ifdef CSR_SATA_PHY_BASE

static void sata_require_csrs(void)
{
#if !defined(CSR_SATA_RX_STREAMER_BASE) || !defined(CSR_SATA_TX_STREAMER_BASE) || !defined(CSR_TXRX_LOOPBACK_BASE)
    fprintf(stderr, "SATA blocks not fully present in this gateware.\n");
    exit(1);
#endif
}

static void txrx_loopback_set(void *conn, int enable)
{
    uint32_t v = 0;
    v |= (enable ? 1u : 0u) << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET;
    m2sdr_writel(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR, v);
}

static void sata_rx_program(void *conn, uint64_t sector, uint32_t nsectors)
{
    csr_write64(conn, CSR_SATA_RX_STREAMER_SECTOR_ADDR, sector);
    m2sdr_writel(conn, CSR_SATA_RX_STREAMER_NSECTORS_ADDR, nsectors);
}

static void sata_tx_program(void *conn, uint64_t sector, uint32_t nsectors)
{
    csr_write64(conn, CSR_SATA_TX_STREAMER_SECTOR_ADDR, sector);
    m2sdr_writel(conn, CSR_SATA_TX_STREAMER_NSECTORS_ADDR, nsectors);
}

static void sata_rx_start(void *conn)
{
    m2sdr_writel(conn, CSR_SATA_RX_STREAMER_START_ADDR, 1);
}

static void sata_tx_start(void *conn)
{
    m2sdr_writel(conn, CSR_SATA_TX_STREAMER_START_ADDR, 1);
}

static uint32_t sata_rx_done(void *conn)
{
    return m2sdr_readl(conn, CSR_SATA_RX_STREAMER_DONE_ADDR);
}

static uint32_t sata_tx_done(void *conn)
{
    return m2sdr_readl(conn, CSR_SATA_TX_STREAMER_DONE_ADDR);
}

static uint32_t sata_rx_error(void *conn)
{
    return m2sdr_readl(conn, CSR_SATA_RX_STREAMER_ERROR_ADDR);
}

static uint32_t sata_tx_error(void *conn)
{
    return m2sdr_readl(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR);
}

static void wait_done(const char *name,
                      uint32_t (*done_fn)(void *),
                      uint32_t (*err_fn)(void *),
                      void *conn,
                      int timeout_ms,
                      uint64_t nsectors)
{
    int elapsed = 0;
    int64_t start = get_time_ms();
    int64_t last  = start;
    for (;;) {
        uint32_t done = done_fn(conn);
        uint32_t err  = err_fn(conn);
        if (done) {
            if (err) printf("%s: done (error=1)\n", name);
            else     printf("%s: done\n", name);
            return;
        }
        if (timeout_ms >= 0 && elapsed >= timeout_ms) {
            fprintf(stderr, "%s: timeout\n", name);
            exit(1);
        }
        int64_t now = get_time_ms();
        if (now - last >= 500) {
            double mb = (double)nsectors * 512.0 / (1024.0 * 1024.0);
            double s  = (double)(now - start) / 1000.0;
            double mbps = (s > 0.0) ? (mb / s) : 0.0;
            fprintf(stderr, "%s: in progress (%.1f MB, %.2f MB/s)\n", name, mb, mbps);
            last = now;
        }
        msleep(10);
        elapsed += 10;
    }
}

static void do_record(uint64_t dst_sector, uint32_t nsectors, int timeout_ms)
{
    void *conn = m2sdr_open();
    sata_require_csrs();

    /* Normal path: RX -> demux -> SATA_RX_STREAMER. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        (int)m2sdr_readl(conn, CSR_CROSSBAR_MUX_SEL_ADDR),
        RXDST_SATA
    );

    sata_rx_program(conn, dst_sector, nsectors);
    sata_rx_start(conn);

    wait_done("SATA_RX(record)", sata_rx_done, sata_rx_error, conn, timeout_ms, nsectors);

    m2sdr_close(conn);
}

static void do_play(uint64_t src_sector, uint32_t nsectors, int timeout_ms)
{
    void *conn = m2sdr_open();
    sata_require_csrs();

    /* Normal path: SATA_TX_STREAMER -> mux -> TX. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        TXSRC_SATA,
        (int)m2sdr_readl(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR)
    );

    sata_tx_program(conn, src_sector, nsectors);
    sata_tx_start(conn);

    wait_done("SATA_TX(play)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);

    m2sdr_close(conn);
}

static void do_replay(uint64_t src_sector, uint32_t nsectors, const char *dst_s, int timeout_ms)
{
    void *conn = m2sdr_open();
    sata_require_csrs();

    int rxdst = parse_rxdst(dst_s);

    /* SATA -> TX -> loopback -> RX destination. */
    crossbar_set(conn, TXSRC_SATA, rxdst);
    txrx_loopback_set(conn, 1);

    sata_tx_program(conn, src_sector, nsectors);
    sata_tx_start(conn);

    wait_done("SATA_TX(replay)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);

    m2sdr_close(conn);
}

static void do_copy(uint64_t src_sector, uint64_t dst_sector, uint32_t nsectors, int timeout_ms)
{
    void *conn = m2sdr_open();
    sata_require_csrs();

    /* SSD -> SSD:
     * SATA_TX_STREAMER -> TX -> loopback -> RX -> SATA_RX_STREAMER.
     */
    crossbar_set(conn, TXSRC_SATA, RXDST_SATA);
    txrx_loopback_set(conn, 1);

    /* Start RX first. */
    sata_rx_program(conn, dst_sector, nsectors);
    sata_tx_program(conn, src_sector, nsectors);

    sata_rx_start(conn);
    msleep(5);
    sata_tx_start(conn);

    wait_done("SATA_TX(copy-src)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    wait_done("SATA_RX(copy-dst)", sata_rx_done, sata_rx_error, conn, timeout_ms, nsectors);

    m2sdr_close(conn);
}

#endif /* CSR_SATA_PHY_BASE */

/* Status ------------------------------------------------------------------- */

static void status(void)
{
    void *conn = m2sdr_open();

#ifdef CSR_CROSSBAR_BASE
    uint32_t txsel = m2sdr_readl(conn, CSR_CROSSBAR_MUX_SEL_ADDR);
    uint32_t rxsel = m2sdr_readl(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);
    printf("Crossbar:\n");
    printf("  mux.sel   = %" PRIu32 " (0=pcie,1=eth,2=sata)\n", txsel);
    printf("  demux.sel = %" PRIu32 " (0=pcie,1=eth,2=sata)\n", rxsel);
#else
    printf("Crossbar: not present\n");
#endif

#ifdef CSR_SATA_PHY_BASE
    printf("SATA:\n");
    printf("  phy.enable = %" PRIu32 "\n", m2sdr_readl(conn, CSR_SATA_PHY_ENABLE_ADDR));
    {
        uint32_t st = m2sdr_readl(conn, CSR_SATA_PHY_STATUS_ADDR);
        printf("  phy.status = 0x%08" PRIx32 "\n", st);
        printf("    ready      = %u\n", (st >> CSR_SATA_PHY_STATUS_READY_OFFSET)      & ((1u << CSR_SATA_PHY_STATUS_READY_SIZE)      - 1));
        printf("    tx_ready   = %u\n", (st >> CSR_SATA_PHY_STATUS_TX_READY_OFFSET)   & ((1u << CSR_SATA_PHY_STATUS_TX_READY_SIZE)   - 1));
        printf("    rx_ready   = %u\n", (st >> CSR_SATA_PHY_STATUS_RX_READY_OFFSET)   & ((1u << CSR_SATA_PHY_STATUS_RX_READY_SIZE)   - 1));
        printf("    ctrl_ready = %u\n", (st >> CSR_SATA_PHY_STATUS_CTRL_READY_OFFSET) & ((1u << CSR_SATA_PHY_STATUS_CTRL_READY_SIZE) - 1));
    }

#ifdef CSR_TXRX_LOOPBACK_BASE
    {
        uint32_t v = m2sdr_readl(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR);
        printf("  txrx_loopback.enable = %u\n",
            (v >> CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET) & ((1u << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_SIZE) - 1));
    }
#endif

#ifdef CSR_SATA_TX_STREAMER_BASE
    printf("  sata_tx_streamer: done=%" PRIu32 " error=%" PRIu32 "\n",
        m2sdr_readl(conn, CSR_SATA_TX_STREAMER_DONE_ADDR),
        m2sdr_readl(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR));
#endif
#ifdef CSR_SATA_RX_STREAMER_BASE
    printf("  sata_rx_streamer: done=%" PRIu32 " error=%" PRIu32 "\n",
        m2sdr_readl(conn, CSR_SATA_RX_STREAMER_DONE_ADDR),
        m2sdr_readl(conn, CSR_SATA_RX_STREAMER_ERROR_ADDR));
#endif

#else
    printf("SATA: not present\n");
#endif

    m2sdr_close(conn);
}

/* Route (always available if crossbar exists) ------------------------------ */

static void do_route(const char *txsrc_s, const char *rxdst_s, int loopback_en)
{
    void *conn = m2sdr_open();

    int txsrc = parse_txsrc(txsrc_s);
    int rxdst = parse_rxdst(rxdst_s);

    crossbar_set(conn, txsrc, rxdst);

#ifdef CSR_SATA_PHY_BASE
    if (loopback_en >= 0) {
        /* Only meaningful if loopback CSR is present. */
#ifdef CSR_TXRX_LOOPBACK_BASE
        txrx_loopback_set(conn, loopback_en);
#else
        fprintf(stderr, "TXRX loopback CSR not present.\n");
        exit(1);
#endif
    }
#else
    if (loopback_en >= 0) {
        fprintf(stderr, "SATA/loopback not present in this gateware.\n");
        exit(1);
    }
#endif

    m2sdr_close(conn);
}

/* Help --------------------------------------------------------------------- */

static void help(void)
{
    printf("M2SDR SATA Utility\n"
           "usage: m2sdr_sata [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "-h                               Help.\n"
#ifdef USE_LITEPCIE
           "-c device_num                    Select device (default=0).\n"
#elif defined(USE_LITEETH)
           "-i ip_address                    Target IP address (default=192.168.1.50).\n"
           "-p port                          Etherbone port (default=1234).\n"
#endif
           "-T timeout_ms                    Timeout for streamer ops (default=10000, -1=infinite).\n"
           "\n"
           "commands:\n"
           "status\n"
           "    Show crossbar + (if present) SATA/loopback/streamer status.\n"
           "\n"
           "route <txsrc> <rxdst> [loopback]\n"
           "    Set routing:\n"
           "      txsrc: pcie|eth|sata\n"
           "      rxdst: pcie|eth|sata\n"
           "      loopback: 0|1 (optional, only if SATA present)\n"
           "\n"
#ifdef CSR_SATA_PHY_BASE
           "record <dst_sector> <nsectors>\n"
           "    RX stream -> SSD (SATA_RX_STREAMER).\n"
           "\n"
           "play <src_sector> <nsectors>\n"
           "    SSD -> TX stream (SATA_TX_STREAMER).\n"
           "\n"
           "replay <src_sector> <nsectors> <dst>\n"
           "    SSD -> TX -> loopback -> RX destination.\n"
           "    dst: pcie|eth|sata\n"
           "\n"
           "copy <src_sector> <dst_sector> <nsectors>\n"
           "    SSD -> SSD copy using loopback.\n"
           "\n"
#else
           "record/play/replay/copy\n"
           "    Not available: SATA not present in this gateware.\n"
           "\n"
#endif
           "header <tx|rx|both> <enable> <header_enable>\n"
           "    Raw header control bits (writes CSR_HEADER_*_CONTROL).\n"
           "\n");
    exit(1);
}

/* Main --------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *cmd;
    int c;

    int timeout_ms = 10000;

    for (;;) {
#ifdef USE_LITEPCIE
        c = getopt(argc, argv, "hc:T:");
#elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:T:");
#endif
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            break;
#ifdef USE_LITEPCIE
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
        case 'T':
            timeout_ms = (int)strtol(optarg, NULL, 0);
            break;
        default:
            exit(1);
        }
    }

    if (optind >= argc)
        help();

#ifndef CSR_SATA_PHY_BASE
    (void)timeout_ms;
#endif

#ifdef USE_LITEPCIE
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);
#endif

    cmd = argv[optind++];

    if (!strcmp(cmd, "status")) {
        status();
        return 0;
    }

    if (!strcmp(cmd, "route")) {
        if (optind + 2 > argc) help();
        const char *txsrc = argv[optind++];
        const char *rxdst = argv[optind++];
        int loopback = -1;
        if (optind < argc)
            loopback = (int)parse_u32(argv[optind++]);
        do_route(txsrc, rxdst, loopback);
        return 0;
    }

#ifdef CSR_SATA_PHY_BASE
    if (!strcmp(cmd, "record")) {
        if (optind + 2 > argc) help();
        uint64_t dst_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            fprintf(stderr, "nsectors must be > 0\n");
            return 1;
        }
        do_record(dst_sector, nsectors, timeout_ms);
        return 0;
    }

    if (!strcmp(cmd, "play")) {
        if (optind + 2 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            fprintf(stderr, "nsectors must be > 0\n");
            return 1;
        }
        do_play(src_sector, nsectors, timeout_ms);
        return 0;
    }

    if (!strcmp(cmd, "replay")) {
        if (optind + 3 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        const char *dst     = argv[optind++];
        if (nsectors == 0) {
            fprintf(stderr, "nsectors must be > 0\n");
            return 1;
        }
        do_replay(src_sector, nsectors, dst, timeout_ms);
        return 0;
    }

    if (!strcmp(cmd, "copy")) {
        if (optind + 3 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint64_t dst_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            fprintf(stderr, "nsectors must be > 0\n");
            return 1;
        }
        do_copy(src_sector, dst_sector, nsectors, timeout_ms);
        return 0;
    }
#else
    if (!strcmp(cmd, "record") || !strcmp(cmd, "play") || !strcmp(cmd, "replay") || !strcmp(cmd, "copy")) {
        fprintf(stderr, "Command '%s' not available: SATA not present in this gateware.\n", cmd);
        return 1;
    }
#endif

    if (!strcmp(cmd, "header")) {
        if (optind + 3 > argc) help();
        const char *which_s = argv[optind++];
        int which = 0;
        if (!strcmp(which_s, "tx")) which = 0;
        else if (!strcmp(which_s, "rx")) which = 1;
        else if (!strcmp(which_s, "both")) which = 2;
        else {
            fprintf(stderr, "header: expected tx|rx|both\n");
            return 1;
        }
        int enable        = (int)parse_u32(argv[optind++]);
        int header_enable = (int)parse_u32(argv[optind++]);

        void *conn = m2sdr_open();
        header_set_raw(conn, which, enable, header_enable);
        m2sdr_close(conn);
        return 0;
    }

    help();
    return 0;
}
