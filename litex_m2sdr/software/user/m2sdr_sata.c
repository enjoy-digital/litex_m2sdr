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
#include <getopt.h>

#include "liblitepcie.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "csr.h"

/* Connection options -------------------------------------------------------- */

static struct m2sdr_cli_device g_cli_dev;

/* Helpers ------------------------------------------------------------------- */

static uint64_t parse_u64(const char *s)
{
    char *end = NULL;
    uint64_t v = strtoull(s, &end, 0);
    if (!s || !*s || (end && *end)) {
        m2sdr_cli_error("invalid number '%s'", s ? s : "(null)");
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

static int parse_timeout_ms(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (!s || !*s || (end && *end)) {
        m2sdr_cli_error("invalid timeout '%s'", s ? s : "(null)");
        exit(1);
    }
    if (v < -1 || v > INT32_MAX) {
        m2sdr_cli_error("timeout must be -1 or a non-negative integer");
        exit(1);
    }
    return (int)v;
}

static int parse_bool01(const char *label, const char *s)
{
    uint32_t v = parse_u32(s);
    if (v > 1) {
        m2sdr_cli_invalid_choice(label, s, "0 or 1");
        exit(1);
    }
    return (int)v;
}

static __attribute__((unused)) void msleep(unsigned ms)
{
    usleep(ms * 1000);
}

static int64_t m2sdr_sata_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* Connection functions ------------------------------------------------------ */

static struct m2sdr_dev *m2sdr_open_dev(void)
{
    if (!m2sdr_cli_finalize_device(&g_cli_dev)) {
        exit(1);
    }
    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev)) != 0) {
        fprintf(stderr, "Could not open %s\n", m2sdr_cli_device_id(&g_cli_dev));
        exit(1);
    }
    return dev;
}

static void m2sdr_close_dev(struct m2sdr_dev *dev)
{
    if (dev) {
        m2sdr_close(dev);
    }
}

/* 64-bit CSR access (LiteX ordering: upper @ base+0, lower @ base+4) -------- */

static void csr_write64(struct m2sdr_dev *dev, uint32_t addr, uint64_t v)
{
    m2sdr_reg_write(dev, addr + 0, (uint32_t)(v >> 32));
    m2sdr_reg_write(dev, addr + 4, (uint32_t)(v >>  0));
}

static  __attribute__((unused))  uint64_t csr_read64(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t upper = 0;
    uint32_t lower = 0;
    m2sdr_reg_read(dev, addr + 0, &upper);
    m2sdr_reg_read(dev, addr + 4, &lower);
    return ((uint64_t)upper << 32) | (uint64_t)lower;
}

static uint32_t m2sdr_read32(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t val = 0;
    if (m2sdr_reg_read(dev, addr, &val) != 0) {
        fprintf(stderr, "CSR read failed @0x%08x\n", addr);
        exit(1);
    }
    return val;
}

static void m2sdr_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (m2sdr_reg_write(dev, addr, val) != 0) {
        fprintf(stderr, "CSR write failed @0x%08x\n", addr);
        exit(1);
    }
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

#ifdef CSR_TXRX_LOOPBACK_BASE
static void txrx_loopback_set(void *conn, int enable);
#endif

static int parse_txsrc(const char *s)
{
    if (!strcmp(s, "pcie")) return TXSRC_PCIE;
    if (!strcmp(s, "eth"))  return TXSRC_ETH;
    if (!strcmp(s, "sata")) return TXSRC_SATA;
    m2sdr_cli_invalid_choice("TX source", s, "pcie, eth, or sata");
    exit(1);
}

static int parse_rxdst(const char *s)
{
    if (!strcmp(s, "pcie")) return RXDST_PCIE;
    if (!strcmp(s, "eth"))  return RXDST_ETH;
    if (!strcmp(s, "sata")) return RXDST_SATA;
    m2sdr_cli_invalid_choice("RX destination", s, "pcie, eth, or sata");
    exit(1);
}

static const char *txsrc_name(int txsrc)
{
    switch (txsrc) {
    case TXSRC_PCIE: return "pcie";
    case TXSRC_ETH:  return "eth";
    case TXSRC_SATA: return "sata";
    default:         return "unknown";
    }
}

static const char *rxdst_name(int rxdst)
{
    switch (rxdst) {
    case RXDST_PCIE: return "pcie";
    case RXDST_ETH:  return "eth";
    case RXDST_SATA: return "sata";
    default:         return "unknown";
    }
}

static void crossbar_set(void *conn, int txsrc, int rxdst)
{
#ifdef CSR_CROSSBAR_BASE
    m2sdr_write32(conn, CSR_CROSSBAR_MUX_SEL_ADDR,   (uint32_t)txsrc);
    m2sdr_write32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR, (uint32_t)rxdst);
#else
    (void)conn; (void)txsrc; (void)rxdst;
    fprintf(stderr, "Crossbar CSR not present in this gateware.\n");
    exit(1);
#endif
}

struct sata_route_state {
    bool crossbar_valid;
    bool loopback_valid;
    int txsrc;
    int rxdst;
    int loopback_en;
};

static struct sata_route_state sata_route_state_capture(void *conn)
{
    struct sata_route_state state = {0};
#ifdef CSR_CROSSBAR_BASE
    state.crossbar_valid = true;
    state.txsrc = (int)m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);
    state.rxdst = (int)m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);
#else
    (void)conn;
#endif
#ifdef CSR_TXRX_LOOPBACK_BASE
    state.loopback_valid = true;
    state.loopback_en =
        (int)((m2sdr_read32(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR) >>
              CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET) &
              ((1u << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_SIZE) - 1));
#endif
    return state;
}

static void sata_route_state_restore(void *conn, const struct sata_route_state *state)
{
    if (state->crossbar_valid)
        crossbar_set(conn, state->txsrc, state->rxdst);
#ifdef CSR_TXRX_LOOPBACK_BASE
    if (state->loopback_valid)
        txrx_loopback_set(conn, state->loopback_en);
#else
    (void)conn;
    (void)state;
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
        m2sdr_write32(conn, CSR_HEADER_TX_CONTROL_ADDR, v);

    v  = 0;
    v |= (enable        ? 1u : 0u) << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET;
    v |= (header_enable ? 1u : 0u) << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET;
    if (which == 1 || which == 2)
        m2sdr_write32(conn, CSR_HEADER_RX_CONTROL_ADDR, v);
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
    m2sdr_write32(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR, v);
}

static void sata_rx_program(void *conn, uint64_t sector, uint32_t nsectors)
{
    csr_write64(conn, CSR_SATA_RX_STREAMER_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_RX_STREAMER_NSECTORS_ADDR, nsectors);
}

static void sata_tx_program(void *conn, uint64_t sector, uint32_t nsectors)
{
    csr_write64(conn, CSR_SATA_TX_STREAMER_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_TX_STREAMER_NSECTORS_ADDR, nsectors);
}

static void sata_rx_start(void *conn)
{
    m2sdr_write32(conn, CSR_SATA_RX_STREAMER_START_ADDR, 1);
}

static void sata_tx_start(void *conn)
{
    m2sdr_write32(conn, CSR_SATA_TX_STREAMER_START_ADDR, 1);
}

static uint32_t sata_rx_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_RX_STREAMER_DONE_ADDR);
}

static uint32_t sata_tx_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_TX_STREAMER_DONE_ADDR);
}

static uint32_t sata_rx_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_RX_STREAMER_ERROR_ADDR);
}

static uint32_t sata_tx_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR);
}

static void wait_done(const char *name,
                      uint32_t (*done_fn)(void *),
                      uint32_t (*err_fn)(void *),
                      void *conn,
                      int timeout_ms,
                      uint64_t nsectors)
{
    int elapsed = 0;
    int64_t start = m2sdr_sata_get_time_ms();
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
        int64_t now = m2sdr_sata_get_time_ms();
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
    struct m2sdr_dev *conn = m2sdr_open_dev();
    struct sata_route_state saved_route = sata_route_state_capture(conn);
    sata_require_csrs();

    /* Normal path: RX -> demux -> SATA_RX_STREAMER. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        (int)m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR),
        RXDST_SATA
    );

    sata_rx_program(conn, dst_sector, nsectors);
    sata_rx_start(conn);

    wait_done("SATA_RX(record)", sata_rx_done, sata_rx_error, conn, timeout_ms, nsectors);
    sata_route_state_restore(conn, &saved_route);

    m2sdr_close_dev(conn);
}

static void do_play(uint64_t src_sector, uint32_t nsectors, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    struct sata_route_state saved_route = sata_route_state_capture(conn);
    sata_require_csrs();

    /* Normal path: SATA_TX_STREAMER -> mux -> TX. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        TXSRC_SATA,
        (int)m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR)
    );

    sata_tx_program(conn, src_sector, nsectors);
    sata_tx_start(conn);

    wait_done("SATA_TX(play)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    sata_route_state_restore(conn, &saved_route);

    m2sdr_close_dev(conn);
}

static void do_replay(uint64_t src_sector, uint32_t nsectors, const char *dst_s, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    struct sata_route_state saved_route = sata_route_state_capture(conn);
    sata_require_csrs();

    int rxdst = parse_rxdst(dst_s);

    /* SATA -> TX -> loopback -> RX destination. */
    crossbar_set(conn, TXSRC_SATA, rxdst);
    txrx_loopback_set(conn, 1);

    sata_tx_program(conn, src_sector, nsectors);
    sata_tx_start(conn);

    wait_done("SATA_TX(replay)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    sata_route_state_restore(conn, &saved_route);

    m2sdr_close_dev(conn);
}

static void do_copy(uint64_t src_sector, uint64_t dst_sector, uint32_t nsectors, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    struct sata_route_state saved_route = sata_route_state_capture(conn);
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
    sata_route_state_restore(conn, &saved_route);

    m2sdr_close_dev(conn);
}

#endif /* CSR_SATA_PHY_BASE */

/* Status ------------------------------------------------------------------- */

static void status(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

#ifdef CSR_CROSSBAR_BASE
    uint32_t txsel = m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);
    uint32_t rxsel = m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);
    printf("Route:\n");
    printf("  TX source          %s (%" PRIu32 ")\n", txsrc_name((int)txsel), txsel);
    printf("  RX destination     %s (%" PRIu32 ")\n", rxdst_name((int)rxsel), rxsel);
#else
    printf("Route:\n");
    printf("  Crossbar           not present\n");
#endif

#ifdef CSR_SATA_PHY_BASE
    printf("SATA:\n");
    printf("  PHY enable         %" PRIu32 "\n", m2sdr_read32(conn, CSR_SATA_PHY_ENABLE_ADDR));
    {
        uint32_t st = m2sdr_read32(conn, CSR_SATA_PHY_STATUS_ADDR);
        uint32_t ready =
            (st >> CSR_SATA_PHY_STATUS_READY_OFFSET) &
            ((1u << CSR_SATA_PHY_STATUS_READY_SIZE) - 1);
        uint32_t tx_ready =
            (st >> CSR_SATA_PHY_STATUS_TX_READY_OFFSET) &
            ((1u << CSR_SATA_PHY_STATUS_TX_READY_SIZE) - 1);
        uint32_t rx_ready =
            (st >> CSR_SATA_PHY_STATUS_RX_READY_OFFSET) &
            ((1u << CSR_SATA_PHY_STATUS_RX_READY_SIZE) - 1);
        uint32_t ctrl_ready =
            (st >> CSR_SATA_PHY_STATUS_CTRL_READY_OFFSET) &
            ((1u << CSR_SATA_PHY_STATUS_CTRL_READY_SIZE) - 1);
        printf("  PHY status         0x%08" PRIx32 "\n", st);
        printf("  PHY ready          %s\n", ready ? "yes" : "no");
        printf("  TX ready           %s\n", tx_ready ? "yes" : "no");
        printf("  RX ready           %s\n", rx_ready ? "yes" : "no");
        printf("  CTRL ready         %s\n", ctrl_ready ? "yes" : "no");
    }

#ifdef CSR_TXRX_LOOPBACK_BASE
    {
        uint32_t v = m2sdr_read32(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR);
        uint32_t loopback =
            (v >> CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET) &
            ((1u << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_SIZE) - 1);
        printf("  Loopback           %s\n", loopback ? "enabled" : "disabled");
    }
#endif

#ifdef CSR_SATA_TX_STREAMER_BASE
    printf("  TX streamer        done=%" PRIu32 " error=%" PRIu32 "\n",
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_DONE_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR));
#endif
#ifdef CSR_SATA_RX_STREAMER_BASE
    printf("  RX streamer        done=%" PRIu32 " error=%" PRIu32 "\n",
        m2sdr_read32(conn, CSR_SATA_RX_STREAMER_DONE_ADDR),
        m2sdr_read32(conn, CSR_SATA_RX_STREAMER_ERROR_ADDR));
#endif

#else
    printf("SATA:\n");
    printf("  not present\n");
#endif

    m2sdr_close_dev(conn);
}

/* Route (always available if crossbar exists) ------------------------------ */

static void do_route(const char *txsrc_s, const char *rxdst_s, int loopback_en)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

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

    m2sdr_close_dev(conn);
}

/* Help --------------------------------------------------------------------- */

static void help(void)
{
    printf("M2SDR SATA Utility\n"
           "usage: m2sdr_sata [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "  -h, --help                     Show this help message.\n"
           "  -d, --device DEV               Use explicit device id.\n"
#ifdef USE_LITEPCIE
           "  -c, --device-num N             Select the device (default: 0).\n"
#elif defined(USE_LITEETH)
           "  -i, --ip ADDR                  Target IP address for Etherbone.\n"
           "  -p, --port PORT                Port number (default: 1234).\n"
#endif
           "      --timeout-ms MS            Timeout for streamer operations (default: 10000, -1=infinite).\n"
           "\n"
           "commands:\n"
           "status\n"
           "    Show crossbar + (if present) SATA/loopback/streamer status.\n"
           "\n"
           "route TXSRC RXDST [LOOPBACK]\n"
           "    Set routing:\n"
           "      txsrc: pcie|eth|sata\n"
           "      rxdst: pcie|eth|sata\n"
           "      loopback: 0|1 (optional, only if SATA present)\n"
           "\n"
#ifdef CSR_SATA_PHY_BASE
           "record DST_SECTOR NSECTORS\n"
           "    RX stream -> SSD (SATA_RX_STREAMER).\n"
           "\n"
           "play SRC_SECTOR NSECTORS\n"
           "    SSD -> TX stream (SATA_TX_STREAMER).\n"
           "\n"
           "replay SRC_SECTOR NSECTORS DST\n"
           "    SSD -> TX -> loopback -> RX destination.\n"
           "    dst: pcie|eth|sata\n"
           "\n"
           "copy SRC_SECTOR DST_SECTOR NSECTORS\n"
           "    SSD -> SSD copy using loopback.\n"
           "\n"
#else
           "record/play/replay/copy\n"
           "    Not available: SATA not present in this gateware.\n"
           "\n"
#endif
           "header TX|RX|BOTH ENABLE HEADER_ENABLE\n"
           "    Raw header control bits (writes CSR_HEADER_*_CONTROL).\n"
           "\n");
    exit(1);
}

/* Main --------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *cmd;
    int c;
    int option_index = 0;

    int timeout_ms = 10000;
    m2sdr_cli_device_init(&g_cli_dev);
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "timeout-ms", required_argument, NULL, 'T' },
        { NULL, 0, NULL, 0 }
    };

    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:p:T:", options, &option_index);
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd':
        case 'c':
        case 'i':
        case 'p':
            if (m2sdr_cli_handle_device_option(&g_cli_dev, c, optarg) != 0)
                exit(1);
            break;
        case 'T':
            timeout_ms = parse_timeout_ms(optarg);
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
            loopback = parse_bool01("loopback", argv[optind++]);
        do_route(txsrc, rxdst, loopback);
        return 0;
    }

#ifdef CSR_SATA_PHY_BASE
    if (!strcmp(cmd, "record")) {
        if (optind + 2 > argc) help();
        uint64_t dst_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
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
            m2sdr_cli_error("nsectors must be greater than zero");
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
            m2sdr_cli_error("nsectors must be greater than zero");
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
            m2sdr_cli_error("nsectors must be greater than zero");
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
            m2sdr_cli_invalid_choice("header target", which_s, "tx, rx, or both");
            return 1;
        }
        int enable        = parse_bool01("header enable", argv[optind++]);
        int header_enable = parse_bool01("header header_enable", argv[optind++]);

        struct m2sdr_dev *conn = m2sdr_open_dev();
        header_set_raw(conn, which, enable, header_enable);
        m2sdr_close_dev(conn);
        return 0;
    }

    help();
    return 0;
}
