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
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <math.h>

#include "liblitepcie.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "csr.h"
#include "mem.h"

/* Connection options -------------------------------------------------------- */

static struct m2sdr_cli_device g_cli_dev;
static sig_atomic_t keep_running = 1;
static int g_sata_lock_fd = -1;
static bool g_no_bulk_etherbone = false;

#define ETHERBONE_BULK_WORDS 128u

static void int_handler(int dummy)
{
    (void)dummy;
    keep_running = 0;
}

/* Helpers ------------------------------------------------------------------- */

static uint64_t parse_u64(const char *s)
{
    uint64_t v = 0;

    if (m2sdr_cli_parse_u64(s, &v) != 0) {
        m2sdr_cli_error("invalid number '%s'", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;

    if (m2sdr_cli_parse_u32(s, &v) != 0) {
        m2sdr_cli_error("invalid u32 value '%s'", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static uint32_t parse_u32_range_arg(const char *label, const char *s, uint32_t min, uint32_t max)
{
    uint32_t v = 0;

    if (m2sdr_cli_parse_u32(s, &v) != 0 || v < min || v > max) {
        m2sdr_cli_error("invalid %s '%s' (expected %" PRIu32 "..%" PRIu32 ")",
                        label ? label : "value", s ? s : "(null)", min, max);
        exit(1);
    }
    return v;
}

static int parse_timeout_ms(const char *s)
{
    int v = 0;

    if (m2sdr_cli_parse_int_range(s, -1, INT32_MAX, &v) != 0) {
        m2sdr_cli_error("invalid timeout '%s'", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static int parse_bool01(const char *label, const char *s)
{
    unsigned v = 0;

    if (m2sdr_cli_parse_uint_range(s, 0, 1, &v) != 0) {
        m2sdr_cli_invalid_choice(label, s, "0 or 1");
        exit(1);
    }
    return (int)v;
}

static int64_t parse_i64(const char *label, const char *s)
{
    int64_t v = 0;

    if (m2sdr_cli_parse_int64(s, &v) != 0) {
        m2sdr_cli_error("invalid %s '%s'", label ? label : "value", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static double parse_double_arg(const char *label, const char *s)
{
    double v = 0.0;

    if (m2sdr_cli_parse_double(s, &v) != 0) {
        m2sdr_cli_error("invalid %s '%s'", label ? label : "value", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static const char *format_name(enum m2sdr_format format)
{
    return m2sdr_cli_format_name(format);
}

static const char *channel_layout_name(enum m2sdr_channel_layout layout)
{
    switch (layout) {
    case M2SDR_CHANNEL_LAYOUT_1T1R: return "1t1r";
    case M2SDR_CHANNEL_LAYOUT_2T2R: return "2t2r";
    default:                        return "unknown";
    }
}

static enum m2sdr_channel_layout parse_channel_layout(const char *text)
{
    if (text && strcmp(text, "1t1r") == 0)
        return M2SDR_CHANNEL_LAYOUT_1T1R;
    if (text && strcmp(text, "2t2r") == 0)
        return M2SDR_CHANNEL_LAYOUT_2T2R;
    m2sdr_cli_invalid_choice("channel layout", text, "1t1r or 2t2r");
    exit(1);
}

static unsigned channel_layout_count(enum m2sdr_channel_layout layout)
{
    return layout == M2SDR_CHANNEL_LAYOUT_1T1R ? 1u : 2u;
}

/* Connection functions ------------------------------------------------------ */

static void m2sdr_sata_unlock(void)
{
    if (g_sata_lock_fd >= 0) {
        flock(g_sata_lock_fd, LOCK_UN);
        close(g_sata_lock_fd);
        g_sata_lock_fd = -1;
    }
}

static int m2sdr_sata_lock(void)
{
    if (g_sata_lock_fd >= 0)
        return 0;

    g_sata_lock_fd = open("/tmp/m2sdr_sata.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_sata_lock_fd < 0) {
        perror("/tmp/m2sdr_sata.lock");
        return -1;
    }

    if (flock(g_sata_lock_fd, LOCK_EX) != 0) {
        perror("flock");
        m2sdr_sata_unlock();
        return -1;
    }
    return 0;
}

static struct m2sdr_dev *m2sdr_open_dev(void)
{
    char identifier[M2SDR_IDENT_MAX];
    int rc;
    if (!m2sdr_cli_finalize_device(&g_cli_dev)) {
        exit(1);
    }
    if (m2sdr_sata_lock() != 0)
        exit(1);

    struct m2sdr_dev *dev = NULL;
    rc = m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev));
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "Could not open %s\n", m2sdr_cli_device_id(&g_cli_dev));
        m2sdr_sata_unlock();
        exit(1);
    }
    rc = m2sdr_get_identifier(dev, identifier, sizeof(identifier));
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr,
            "Failed to read SoC identifier. If this is PCIe after loading a "
            "new bitstream, rescan the PCIe bus/driver before using SATA.\n");
        m2sdr_close(dev);
        m2sdr_sata_unlock();
        exit(1);
    }
    return dev;
}

static void m2sdr_close_dev(struct m2sdr_dev *dev)
{
    if (dev) {
        m2sdr_close(dev);
    }
    m2sdr_sata_unlock();
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

#if defined(CSR_SATA_PHY_BASE) && defined(CSR_TXRX_LOOPBACK_BASE)
static void txrx_loopback_set(void *conn, int enable);
#endif

struct route_choice {
    const char *name;
    int value;
};

static const struct route_choice route_choices[] = {
    { "pcie", TXSRC_PCIE },
    { "eth",  TXSRC_ETH  },
    { "sata", TXSRC_SATA },
};

static int parse_route_choice(const char *label, const char *text)
{
    for (size_t i = 0; i < sizeof(route_choices) / sizeof(route_choices[0]); i++) {
        if (text && strcmp(text, route_choices[i].name) == 0)
            return route_choices[i].value;
    }

    m2sdr_cli_invalid_choice(label, text, "pcie, eth, or sata");
    exit(1);
}

static const char *route_choice_name(int route)
{
    switch (route) {
    case TXSRC_PCIE: return "pcie";
    case TXSRC_ETH:  return "eth";
    case TXSRC_SATA: return "sata";
    default:         return "unknown";
    }
}

static int parse_txsrc(const char *s)
{
    return parse_route_choice("TX source", s);
}

static int parse_rxdst(const char *s)
{
    return parse_route_choice("RX destination", s);
}

static const char *txsrc_name(int txsrc)
{
    return route_choice_name(txsrc);
}

static const char *rxdst_name(int rxdst)
{
    return route_choice_name(rxdst);
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

#ifdef CSR_SATA_PHY_BASE

static void msleep(unsigned ms)
{
    usleep(ms * 1000);
}

static void sata_wait_sleep(int64_t elapsed_us)
{
    if (elapsed_us < 2000)
        return;
    if (elapsed_us < 20000) {
        usleep(100);
        return;
    }
    if (elapsed_us < 100000) {
        usleep(1000);
        return;
    }
    msleep(10);
}

static int64_t m2sdr_sata_get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* 64-bit CSR access (LiteX ordering: upper @ base+0, lower @ base+4). */
static void csr_write64(struct m2sdr_dev *dev, uint32_t addr, uint64_t v)
{
    m2sdr_reg_write(dev, addr + 0, (uint32_t)(v >> 32));
    m2sdr_reg_write(dev, addr + 4, (uint32_t)(v >>  0));
}

static uint64_t csr_read64(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t upper = 0;
    uint32_t lower = 0;
    m2sdr_reg_read(dev, addr + 0, &upper);
    m2sdr_reg_read(dev, addr + 4, &lower);
    return ((uint64_t)upper << 32) | (uint64_t)lower;
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

struct sata_operation {
    struct m2sdr_dev *conn;
    struct sata_route_state saved_route;
};

enum sata_wait_result {
    SATA_WAIT_OK = 0,
    SATA_WAIT_TIMEOUT = 1,
    SATA_WAIT_INTERRUPTED = 2,
};

enum sata_pattern_kind {
    SATA_PATTERN_ZERO = 0,
    SATA_PATTERN_COUNTER,
    SATA_PATTERN_PRBS,
};

#define SATA_SECTOR_BYTES 512u
#define SATA_CATALOG_SECTOR  0x800ull
#define SATA_CATALOG_SECTORS 64u
#define SATA_DATA_START      0x100000ull
#define SATA_CAPTURE_NAME_MAX 64
#define SATA_CAPTURE_NOTES_MAX 128
#define SATA_CATALOG_MAX_ENTRIES 64
#if defined(SATA_HOST_BUFFER_BASE) && defined(SATA_HOST_BUFFER_SIZE) && \
    defined(CSR_SATA_SECTOR2MEM_BASE) && defined(CSR_SATA_MEM2SECTOR_BASE)
#define SATA_HOST_IO_AVAILABLE 1
#endif
#if defined(CSR_MAIN_SATA_STREAMER_CONTROL_ADDR)
#define M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR            CSR_MAIN_SATA_STREAMER_CONTROL_ADDR
#define M2SDR_CSR_SATA_STREAMER_CONTROL_RX_RESET_OFFSET CSR_MAIN_SATA_STREAMER_CONTROL_RX_RESET_OFFSET
#define M2SDR_CSR_SATA_STREAMER_CONTROL_TX_RESET_OFFSET CSR_MAIN_SATA_STREAMER_CONTROL_TX_RESET_OFFSET
#elif defined(CSR_SATA_STREAMER_CONTROL_ADDR)
#define M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR            CSR_SATA_STREAMER_CONTROL_ADDR
#define M2SDR_CSR_SATA_STREAMER_CONTROL_RX_RESET_OFFSET CSR_SATA_STREAMER_CONTROL_RX_RESET_OFFSET
#define M2SDR_CSR_SATA_STREAMER_CONTROL_TX_RESET_OFFSET CSR_SATA_STREAMER_CONTROL_TX_RESET_OFFSET
#endif
#endif /* CSR_SATA_PHY_BASE */

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

static struct sata_operation sata_operation_begin(void)
{
    struct sata_operation op;
    op.conn = m2sdr_open_dev();
    op.saved_route = sata_route_state_capture(op.conn);
    sata_require_csrs();
    return op;
}

static void sata_operation_finish(struct sata_operation *op)
{
    sata_route_state_restore(op->conn, &op->saved_route);
    m2sdr_close_dev(op->conn);
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

static int sata_streamers_reset(void *conn, bool rx, bool tx)
{
#ifdef M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR
    uint32_t control = 0;

    if (rx)
        control |= 1u << M2SDR_CSR_SATA_STREAMER_CONTROL_RX_RESET_OFFSET;
    if (tx)
        control |= 1u << M2SDR_CSR_SATA_STREAMER_CONTROL_TX_RESET_OFFSET;
    m2sdr_write32(conn, M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR, control);
    return 0;
#else
    (void)conn; (void)rx; (void)tx;
    return -1;
#endif
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

#ifdef SATA_HOST_IO_AVAILABLE
static uint32_t sata_sector2mem_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_SECTOR2MEM_DONE_ADDR);
}

static uint32_t sata_sector2mem_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_SECTOR2MEM_ERROR_ADDR);
}

static uint32_t sata_mem2sector_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_MEM2SECTOR_DONE_ADDR);
}

static uint32_t sata_mem2sector_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_MEM2SECTOR_ERROR_ADDR);
}
#endif

static enum sata_wait_result wait_done_report(const char *name,
                                              uint32_t (*done_fn)(void *),
                                              uint32_t (*err_fn)(void *),
                                              void *conn,
                                              int timeout_ms,
                                              uint64_t nsectors,
                                              bool report)
{
    int64_t start_us = m2sdr_sata_get_time_us();
    int64_t last_report_us = start_us;

    for (;;) {
        int64_t now_us;
        int64_t elapsed_us;

        if (!keep_running) {
            fprintf(stderr, "%s: interrupted\n", name);
            return SATA_WAIT_INTERRUPTED;
        }

        uint32_t done = done_fn(conn);
        if (done) {
            uint32_t err = err_fn(conn);
            if (report) {
                if (err) printf("%s: done (error=1)\n", name);
                else     printf("%s: done\n", name);
            }
            return SATA_WAIT_OK;
        }

        now_us = m2sdr_sata_get_time_us();
        elapsed_us = now_us - start_us;
        if (timeout_ms >= 0 && elapsed_us >= (int64_t)timeout_ms * 1000) {
            uint32_t err = err_fn(conn);
            if (err)
                fprintf(stderr, "%s: timeout (error=1)\n", name);
            else
                fprintf(stderr, "%s: timeout\n", name);
            return SATA_WAIT_TIMEOUT;
        }

        if (report && now_us - last_report_us >= 500000) {
            double mb = (double)nsectors * 512.0 / (1024.0 * 1024.0);
            double s  = (double)elapsed_us / 1000000.0;
            double mbps = (s > 0.0) ? (mb / s) : 0.0;
            fprintf(stderr, "%s: in progress (%.1f MB, %.2f MB/s)\n", name, mb, mbps);
            last_report_us = now_us;
        }

        sata_wait_sleep(elapsed_us);
    }
}

static enum sata_wait_result wait_done(const char *name,
                                       uint32_t (*done_fn)(void *),
                                       uint32_t (*err_fn)(void *),
                                       void *conn,
                                       int timeout_ms,
                                       uint64_t nsectors)
{
    return wait_done_report(name, done_fn, err_fn, conn, timeout_ms, nsectors, true);
}

static enum sata_wait_result wait_done_quiet(const char *name,
                                             uint32_t (*done_fn)(void *),
                                             uint32_t (*err_fn)(void *),
                                             void *conn,
                                             int timeout_ms)
{
    return wait_done_report(name, done_fn, err_fn, conn, timeout_ms, 1, false);
}

static void print_planned_transfer(const char *name,
                                   uint64_t src_sector,
                                   uint64_t dst_sector,
                                   uint32_t nsectors,
                                   int txsrc,
                                   int rxdst,
                                   int loopback,
                                   int timeout_ms)
{
    printf("%s dry-run:\n", name);
    if (src_sector != UINT64_MAX)
        printf("  Source sector      0x%016" PRIx64 "\n", src_sector);
    if (dst_sector != UINT64_MAX)
        printf("  Destination sector 0x%016" PRIx64 "\n", dst_sector);
    printf("  Sector count       %" PRIu32 "\n", nsectors);
    printf("  TX source          %s\n", txsrc_name(txsrc));
    printf("  RX destination     %s\n", rxdst_name(rxdst));
    printf("  Loopback           %s\n", loopback ? "enabled" : "disabled");
    printf("  Timeout            %d ms\n", timeout_ms);
}

static enum sata_pattern_kind parse_pattern(const char *text)
{
    if (!text || !strcmp(text, "counter"))
        return SATA_PATTERN_COUNTER;
    if (!strcmp(text, "zero"))
        return SATA_PATTERN_ZERO;
    if (!strcmp(text, "prbs"))
        return SATA_PATTERN_PRBS;

    m2sdr_cli_invalid_choice("pattern", text, "zero, counter, or prbs");
    exit(1);
}

#ifdef SATA_HOST_IO_AVAILABLE
static uint32_t pattern_word(enum sata_pattern_kind pattern, uint64_t word_index)
{
    uint64_t x;

    switch (pattern) {
    case SATA_PATTERN_ZERO:
        return 0;
    case SATA_PATTERN_COUNTER:
        return (uint32_t)word_index;
    case SATA_PATTERN_PRBS:
    default:
        x = word_index + 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x = x ^ (x >> 31);
        return (uint32_t)(x ^ (x >> 32));
    }
}

static void put_le32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)((value >>  0) & 0xffu);
    buf[1] = (uint8_t)((value >>  8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t get_le32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] <<  0) |
           ((uint32_t)buf[1] <<  8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static void fill_pattern(uint8_t *buf, size_t len, uint64_t start_byte,
                         enum sata_pattern_kind pattern)
{
    uint64_t word_index = start_byte / 4u;

    for (size_t off = 0; off < len; off += 4)
        put_le32(&buf[off], pattern_word(pattern, word_index++));
}

static bool check_pattern(const uint8_t *buf, size_t len, uint64_t start_byte,
                          enum sata_pattern_kind pattern, uint64_t *bad_offset,
                          uint32_t *expected, uint32_t *actual)
{
    uint64_t word_index = start_byte / 4u;

    for (size_t off = 0; off < len; off += 4) {
        uint32_t exp = pattern_word(pattern, word_index++);
        uint32_t got = get_le32(&buf[off]);
        if (exp != got) {
            if (bad_offset) *bad_offset = start_byte + off;
            if (expected)   *expected = exp;
            if (actual)     *actual = got;
            return false;
        }
    }
    return true;
}

static uint32_t sata_host_buffer_max_sectors(void)
{
    return (uint32_t)(SATA_HOST_BUFFER_SIZE / SATA_SECTOR_BYTES);
}

static bool etherbone_is_liteeth(struct m2sdr_dev *conn)
{
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;

    return m2sdr_get_transport(conn, &transport) == M2SDR_ERR_OK &&
           transport == M2SDR_TRANSPORT_KIND_LITEETH;
}

static void etherbone_fill_test_words(uint32_t *words, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        words[i] = 0x5a000000u ^ (count << 12) ^ (i * 0x01020408u) ^ i;
}

static bool etherbone_probe_burst_words(struct m2sdr_dev *conn, uint32_t count, bool verbose)
{
    uint32_t tx[ETHERBONE_BULK_WORDS];
    uint32_t rx[ETHERBONE_BULK_WORDS];

    if (count == 0 || count > ETHERBONE_BULK_WORDS ||
        (size_t)count * sizeof(uint32_t) > SATA_HOST_BUFFER_SIZE) {
        if (verbose)
            fprintf(stderr, "Etherbone bulk probe: invalid count %" PRIu32 "\n", count);
        return false;
    }

    etherbone_fill_test_words(tx, count);
    memset(rx, 0, count * sizeof(uint32_t));

    if (m2sdr_reg_write_bulk(conn, SATA_HOST_BUFFER_BASE, tx, count) != M2SDR_ERR_OK) {
        if (verbose)
            fprintf(stderr, "Etherbone bulk probe: write failed at %" PRIu32 " words\n", count);
        return false;
    }
    if (m2sdr_reg_read_bulk(conn, SATA_HOST_BUFFER_BASE, rx, count) != M2SDR_ERR_OK) {
        if (verbose)
            fprintf(stderr, "Etherbone bulk probe: read failed at %" PRIu32 " words\n", count);
        return false;
    }
    if (memcmp(tx, rx, count * sizeof(uint32_t)) != 0) {
        if (verbose) {
            for (uint32_t i = 0; i < count; i++) {
                if (tx[i] != rx[i]) {
                    fprintf(stderr,
                            "Etherbone bulk probe: mismatch at %" PRIu32
                            " words index %" PRIu32 " expected=0x%08" PRIx32
                            " actual=0x%08" PRIx32 "\n",
                            count, i, tx[i], rx[i]);
                    break;
                }
            }
        }
        return false;
    }

    return true;
}

static uint32_t sata_host_buffer_bulk_words(struct m2sdr_dev *conn)
{
    static bool probed = false;
    static uint32_t cached_words = 1;

    if (g_no_bulk_etherbone)
        return 1;
    if (probed)
        return cached_words;
    if (!etherbone_is_liteeth(conn)) {
        cached_words = ETHERBONE_BULK_WORDS;
        probed = true;
        return cached_words;
    }

    probed = true;
    if (etherbone_probe_burst_words(conn, ETHERBONE_BULK_WORDS, true)) {
        cached_words = ETHERBONE_BULK_WORDS;
        fprintf(stderr, "Etherbone bulk burst: %" PRIu32 " words\n", cached_words);
        return cached_words;
    }

    cached_words = 1;
    fprintf(stderr, "Etherbone bulk burst probe failed; using legacy single-word access.\n");
    return cached_words;
}

static void sata_host_buffer_write(struct m2sdr_dev *conn, const uint8_t *buf, size_t bytes)
{
    uint32_t burst = sata_host_buffer_bulk_words(conn);
    uint32_t words[ETHERBONE_BULK_WORDS];

    for (size_t off = 0; off < bytes; ) {
        size_t remaining_words = (bytes - off) / sizeof(uint32_t);
        size_t chunk_words = remaining_words < burst ? remaining_words : burst;

        if (chunk_words == 0)
            break;
        if (chunk_words == 1) {
            m2sdr_write32(conn, SATA_HOST_BUFFER_BASE + (uint32_t)off, get_le32(&buf[off]));
        } else {
            uint32_t addr = SATA_HOST_BUFFER_BASE + (uint32_t)off;

            for (size_t i = 0; i < chunk_words; i++)
                words[i] = get_le32(&buf[off + 4 * i]);
            if (m2sdr_reg_write_bulk(conn, addr, words, chunk_words) != M2SDR_ERR_OK) {
                fprintf(stderr, "Host buffer bulk write failed @0x%08" PRIx32 "\n", addr);
                exit(1);
            }
        }
        off += chunk_words * sizeof(uint32_t);
    }
}

static void sata_host_buffer_read(struct m2sdr_dev *conn, uint8_t *buf, size_t bytes)
{
    uint32_t burst = sata_host_buffer_bulk_words(conn);
    uint32_t words[ETHERBONE_BULK_WORDS];

    for (size_t off = 0; off < bytes; ) {
        size_t remaining_words = (bytes - off) / sizeof(uint32_t);
        size_t chunk_words = remaining_words < burst ? remaining_words : burst;

        if (chunk_words == 0)
            break;
        if (chunk_words == 1) {
            put_le32(&buf[off], m2sdr_read32(conn, SATA_HOST_BUFFER_BASE + (uint32_t)off));
        } else {
            uint32_t addr = SATA_HOST_BUFFER_BASE + (uint32_t)off;

            if (m2sdr_reg_read_bulk(conn, addr, words, chunk_words) != M2SDR_ERR_OK) {
                fprintf(stderr, "Host buffer bulk read failed @0x%08" PRIx32 "\n", addr);
                exit(1);
            }
            for (size_t i = 0; i < chunk_words; i++)
                put_le32(&buf[off + 4 * i], words[i]);
        }
        off += chunk_words * sizeof(uint32_t);
    }
}

static int do_etherbone_bench(int argc, char **argv, int argi)
{
    static const uint32_t candidates[] = {1, 2, 4, 8, 16, 32, 64, ETHERBONE_BULK_WORDS};
    uint32_t iterations = 1024;
    struct m2sdr_dev *conn;
    int status = 0;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--iterations")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "Missing value for --iterations\n");
                return 1;
            }
            iterations = parse_u32_range_arg("iterations", argv[++argi], 1, 1000000);
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    conn = m2sdr_open_dev();
    printf("Etherbone burst benchmark: iterations=%" PRIu32 "\n", iterations);
    printf("%8s %12s %12s %12s %s\n", "words", "bytes", "write MiB/s", "read MiB/s", "status");

    for (size_t ci = 0; ci < sizeof(candidates) / sizeof(candidates[0]) && keep_running; ci++) {
        uint32_t n = candidates[ci];
        uint32_t tx[ETHERBONE_BULK_WORDS];
        uint32_t rx[ETHERBONE_BULK_WORDS];
        uint64_t bytes = (uint64_t)iterations * n * sizeof(uint32_t);
        int64_t start_us;
        int64_t write_us;
        int64_t read_us;
        bool ok = true;

        etherbone_fill_test_words(tx, n);
        memset(rx, 0, n * sizeof(uint32_t));

        if (!etherbone_probe_burst_words(conn, n, true)) {
            ok = false;
        } else {
            start_us = m2sdr_sata_get_time_us();
            for (uint32_t i = 0; i < iterations; i++) {
                if (m2sdr_reg_write_bulk(conn, SATA_HOST_BUFFER_BASE, tx, n) != M2SDR_ERR_OK) {
                    ok = false;
                    break;
                }
            }
            write_us = m2sdr_sata_get_time_us() - start_us;

            if (ok) {
                start_us = m2sdr_sata_get_time_us();
                for (uint32_t i = 0; i < iterations; i++) {
                    if (m2sdr_reg_read_bulk(conn, SATA_HOST_BUFFER_BASE, rx, n) != M2SDR_ERR_OK) {
                        ok = false;
                        break;
                    }
                }
                read_us = m2sdr_sata_get_time_us() - start_us;
            } else {
                read_us = 0;
            }

            if (ok && memcmp(tx, rx, n * sizeof(uint32_t)) != 0)
                ok = false;
        }

        if (ok) {
            double mib = (double)bytes / (1024.0 * 1024.0);
            double write_mibs = write_us > 0 ? mib * 1000000.0 / (double)write_us : 0.0;
            double read_mibs  = read_us  > 0 ? mib * 1000000.0 / (double)read_us  : 0.0;

            printf("%8" PRIu32 " %12" PRIu64 " %12.3f %12.3f ok\n",
                   n, bytes, write_mibs, read_mibs);
        } else {
            status = 1;
            printf("%8" PRIu32 " %12" PRIu64 " %12s %12s fail\n",
                   n, bytes, "-", "-");
            break;
        }
        fflush(stdout);
    }

    m2sdr_close_dev(conn);
    return status;
}

static void sata_sector2mem_program(void *conn, uint64_t sector, uint32_t nsectors, uint64_t base)
{
    csr_write64(conn, CSR_SATA_SECTOR2MEM_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_SECTOR2MEM_NSECTORS_ADDR, nsectors);
    csr_write64(conn, CSR_SATA_SECTOR2MEM_BASE_ADDR, base);
}

static void sata_mem2sector_program(void *conn, uint64_t sector, uint32_t nsectors, uint64_t base)
{
    csr_write64(conn, CSR_SATA_MEM2SECTOR_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_MEM2SECTOR_NSECTORS_ADDR, nsectors);
    csr_write64(conn, CSR_SATA_MEM2SECTOR_BASE_ADDR, base);
}

static int sata_read_to_host_buffer(struct m2sdr_dev *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    (void)sata_host_buffer_bulk_words(conn);
    sata_sector2mem_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_SECTOR2MEM_START_ADDR, 1);
    return wait_done("SATA_SECTOR2MEM(read-file)",
        sata_sector2mem_done, sata_sector2mem_error, conn, timeout_ms, nsectors) == SATA_WAIT_OK ? 0 : 1;
}

static int sata_write_from_host_buffer(void *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    sata_mem2sector_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_MEM2SECTOR_START_ADDR, 1);
    return wait_done("SATA_MEM2SECTOR(write-file)",
        sata_mem2sector_done, sata_mem2sector_error, conn, timeout_ms, nsectors) == SATA_WAIT_OK ? 0 : 1;
}

static FILE *open_stdio_or_file(const char *path, const char *mode, bool *need_close)
{
    FILE *f;

    *need_close = false;
    if (!strcmp(path, "-"))
        return (mode[0] == 'r') ? stdin : stdout;

    f = fopen(path, mode);
    if (!f)
        perror(path);
    else
        *need_close = true;
    return f;
}

static int do_read_file(uint64_t src_sector, uint32_t nsectors, const char *path, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t max_sectors = sata_host_buffer_max_sectors();
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    bool close_out = false;
    FILE *out;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    out = open_stdio_or_file(path, "wb", &close_out);
    if (!out)
        goto out_free;

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;

        if (sata_read_to_host_buffer(conn, src_sector + done, chunk, timeout_ms) != 0)
            goto out_file;
        sata_host_buffer_read(conn, buf, bytes);
        if (fwrite(buf, 1, bytes, out) != bytes) {
            perror(path);
            goto out_file;
        }
        done += chunk;
    }

    rc = 0;
    printf("Read %" PRIu32 " sectors from 0x%016" PRIx64 " to %s\n",
        nsectors, src_sector, path);

out_file:
    if (close_out)
        fclose(out);
out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

static int do_write_file(const char *path, uint64_t dst_sector, uint32_t nsectors,
                         bool limit_sectors, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t max_sectors = sata_host_buffer_max_sectors();
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    bool close_in = false;
    FILE *in;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    in = open_stdio_or_file(path, "rb", &close_in);
    if (!in)
        goto out_free;

    for (;;) {
        uint32_t chunk = max_sectors;
        size_t bytes;
        size_t got;

        if (limit_sectors) {
            if (done >= nsectors)
                break;
            if (chunk > nsectors - done)
                chunk = nsectors - done;
        }

        bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        memset(buf, 0, bytes);
        got = fread(buf, 1, bytes, in);
        if (got == 0 && !limit_sectors) {
            if (ferror(in))
                perror(path);
            else
                rc = 0;
            break;
        }
        if (ferror(in)) {
            perror(path);
            goto out_file;
        }
        if (!limit_sectors) {
            chunk = (uint32_t)((got + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
            bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        }

        sata_host_buffer_write(conn, buf, bytes);
        if (sata_write_from_host_buffer(conn, dst_sector + done, chunk, timeout_ms) != 0)
            goto out_file;
        done += chunk;

        if (!limit_sectors && got < (size_t)max_sectors * SATA_SECTOR_BYTES) {
            rc = 0;
            break;
        }
    }

    if (limit_sectors)
        rc = 0;
    if (rc == 0)
        printf("Wrote %" PRIu32 " sectors to 0x%016" PRIx64 " from %s\n",
            done, dst_sector, path);

out_file:
    if (close_in)
        fclose(in);
out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

static int do_write_pattern(uint64_t dst_sector, uint32_t nsectors,
                            enum sata_pattern_kind pattern, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t max_sectors = sata_host_buffer_max_sectors();
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;

        fill_pattern(buf, bytes, (dst_sector + done) * SATA_SECTOR_BYTES, pattern);
        sata_host_buffer_write(conn, buf, bytes);
        if (sata_write_from_host_buffer(conn, dst_sector + done, chunk, timeout_ms) != 0)
            goto out_free;
        done += chunk;
    }

    rc = 0;
    printf("Wrote pattern to %" PRIu32 " sectors at 0x%016" PRIx64 "\n",
        nsectors, dst_sector);

out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

static int do_verify_pattern(uint64_t src_sector, uint32_t nsectors,
                             enum sata_pattern_kind pattern, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t max_sectors = sata_host_buffer_max_sectors();
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        uint64_t bad_offset = 0;
        uint32_t expected = 0;
        uint32_t actual = 0;

        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;

        if (sata_read_to_host_buffer(conn, src_sector + done, chunk, timeout_ms) != 0)
            goto out_free;
        sata_host_buffer_read(conn, buf, bytes);
        if (!check_pattern(buf, bytes, (src_sector + done) * SATA_SECTOR_BYTES,
                           pattern, &bad_offset, &expected, &actual)) {
            fprintf(stderr,
                "Pattern mismatch at byte 0x%016" PRIx64 ": expected=0x%08" PRIx32
                " actual=0x%08" PRIx32 "\n",
                bad_offset, expected, actual);
            goto out_free;
        }
        done += chunk;
    }

    rc = 0;
    printf("Verified pattern over %" PRIu32 " sectors at 0x%016" PRIx64 "\n",
        nsectors, src_sector);

out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

/* Named capture catalog ----------------------------------------------------- */

struct sata_capture_entry {
    bool used;
    char name[SATA_CAPTURE_NAME_MAX];
    uint64_t sector;
    uint32_t nsectors;
    uint64_t bytes;
    int64_t sample_rate;
    char format[16];
    char channel_layout[16];
    uint64_t rx_freq;
    uint64_t tx_freq;
    uint64_t bandwidth;
    int64_t rx_gain;
    int64_t tx_att;
    uint64_t created;
    char notes[SATA_CAPTURE_NOTES_MAX];
};

struct sata_catalog {
    bool initialized;
    struct sata_capture_entry entries[SATA_CATALOG_MAX_ENTRIES];
};

static int sata_read_to_host_buffer_quiet(struct m2sdr_dev *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    (void)sata_host_buffer_bulk_words(conn);
    sata_sector2mem_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_SECTOR2MEM_START_ADDR, 1);
    return wait_done_quiet("SATA_SECTOR2MEM(catalog)",
        sata_sector2mem_done, sata_sector2mem_error, conn, timeout_ms) == SATA_WAIT_OK ? 0 : 1;
}

static int sata_write_from_host_buffer_quiet(void *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    sata_mem2sector_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_MEM2SECTOR_START_ADDR, 1);
    return wait_done_quiet("SATA_MEM2SECTOR(catalog)",
        sata_mem2sector_done, sata_mem2sector_error, conn, timeout_ms) == SATA_WAIT_OK ? 0 : 1;
}

static int catalog_buffer_check(void)
{
    if (SATA_CATALOG_SECTORS > sata_host_buffer_max_sectors()) {
        fprintf(stderr, "Catalog region is larger than SATA host buffer.\n");
        return 1;
    }
    return 0;
}

static void catalog_clear(struct sata_catalog *cat)
{
    memset(cat, 0, sizeof(*cat));
    cat->initialized = true;
}

static int catalog_name_valid(const char *name)
{
    if (!name || !name[0] || strlen(name) >= SATA_CAPTURE_NAME_MAX)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '|' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ' ')
            return 0;
    }
    return 1;
}

static int catalog_text_valid(const char *text, size_t max_len)
{
    if (!text)
        return 1;
    if (strlen(text) >= max_len)
        return 0;
    for (const char *p = text; *p; p++) {
        if (*p == '|' || *p == '\n' || *p == '\r')
            return 0;
    }
    return 1;
}

static void catalog_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dst_len, "%s", src);
}

static char *catalog_next_field(char **save)
{
    char *field = *save;
    char *sep;

    if (!field)
        return NULL;
    sep = strchr(field, '|');
    if (sep) {
        *sep = '\0';
        *save = sep + 1;
    } else {
        *save = NULL;
    }
    return field;
}

static int catalog_parse_entry(struct sata_catalog *cat, char *line)
{
    char *save = line;
    char *field;
    struct sata_capture_entry e;
    int slot = -1;

    memset(&e, 0, sizeof(e));
    field = catalog_next_field(&save);
    if (!field || strcmp(field, "entry") != 0)
        return 0;

    field = catalog_next_field(&save);
    if (!catalog_name_valid(field))
        return -1;
    catalog_copy(e.name, sizeof(e.name), field);

    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.sector) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u32(field, &e.nsectors) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.bytes) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.sample_rate) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!catalog_text_valid(field, sizeof(e.format)))
        return -1;
    catalog_copy(e.format, sizeof(e.format), field);
    field = catalog_next_field(&save);
    if (!catalog_text_valid(field, sizeof(e.channel_layout)))
        return -1;
    catalog_copy(e.channel_layout, sizeof(e.channel_layout), field);
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.rx_freq) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.tx_freq) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.bandwidth) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.rx_gain) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.tx_att) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.created) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!catalog_text_valid(field, sizeof(e.notes)))
        return -1;
    catalog_copy(e.notes, sizeof(e.notes), field ? field : "");

    e.used = true;
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        if (!cat->entries[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    cat->entries[slot] = e;
    return 0;
}

static int catalog_load_from_conn(void *conn, struct sata_catalog *cat, int timeout_ms)
{
    uint32_t bytes = SATA_CATALOG_SECTORS * SATA_SECTOR_BYTES;
    uint8_t *buf;
    char *text;
    char *line;
    char *save;
    int rc = 1;

    memset(cat, 0, sizeof(*cat));
    if (catalog_buffer_check() != 0)
        return 1;

    buf = calloc(1, bytes + 1u);
    if (!buf) {
        fprintf(stderr, "Failed to allocate catalog buffer.\n");
        return 1;
    }
    if (sata_read_to_host_buffer_quiet(conn, SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS, timeout_ms) != 0)
        goto out;
    sata_host_buffer_read(conn, buf, bytes);
    text = (char *)buf;
    text[bytes] = '\0';

    line = strtok_r(text, "\n", &save);
    if (!line || strcmp(line, "M2SDR_SATA_CATALOG_V1") != 0) {
        rc = 0;
        goto out;
    }
    cat->initialized = true;
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        if (strncmp(line, "entry|", 6) == 0 && catalog_parse_entry(cat, line) != 0) {
            fprintf(stderr, "Invalid SATA catalog entry.\n");
            goto out;
        }
    }
    rc = 0;

out:
    free(buf);
    return rc;
}

static int catalog_appendf(char *buf, size_t buf_len, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*used >= buf_len)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, buf_len - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= buf_len - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

static int catalog_save_to_conn(void *conn, const struct sata_catalog *cat, int timeout_ms)
{
    uint32_t bytes = SATA_CATALOG_SECTORS * SATA_SECTOR_BYTES;
    uint8_t *buf;
    size_t used = 0;
    int rc = 1;

    if (catalog_buffer_check() != 0)
        return 1;
    buf = calloc(1, bytes);
    if (!buf) {
        fprintf(stderr, "Failed to allocate catalog buffer.\n");
        return 1;
    }

    if (catalog_appendf((char *)buf, bytes, &used,
            "M2SDR_SATA_CATALOG_V1\n"
            "catalog_sector=%" PRIu64 "\n"
            "catalog_sectors=%u\n"
            "data_start=%" PRIu64 "\n",
            (uint64_t)SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS,
            (uint64_t)SATA_DATA_START) != 0)
        goto out;

    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &cat->entries[i];
        if (!e->used)
            continue;
        if (catalog_appendf((char *)buf, bytes, &used,
                "entry|%s|%" PRIu64 "|%" PRIu32 "|%" PRIu64 "|%" PRId64
                "|%s|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRId64
                "|%" PRId64 "|%" PRIu64 "|%s\n",
                e->name, e->sector, e->nsectors, e->bytes, e->sample_rate,
                e->format, e->channel_layout, e->rx_freq, e->tx_freq,
                e->bandwidth, e->rx_gain, e->tx_att, e->created, e->notes) != 0) {
            fprintf(stderr, "SATA catalog is full.\n");
            goto out;
        }
    }

    sata_host_buffer_write(conn, buf, bytes);
    if (sata_write_from_host_buffer_quiet(conn, SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS, timeout_ms) != 0)
        goto out;
    rc = 0;

out:
    free(buf);
    return rc;
}

static int catalog_load(struct sata_catalog *cat, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rc;

    sata_require_csrs();
    rc = catalog_load_from_conn(conn, cat, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static int catalog_save(const struct sata_catalog *cat, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rc;

    sata_require_csrs();
    rc = catalog_save_to_conn(conn, cat, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static int catalog_require(struct sata_catalog *cat, int timeout_ms)
{
    if (catalog_load(cat, timeout_ms) != 0)
        return 1;
    if (!cat->initialized) {
        fprintf(stderr, "SATA catalog is not initialized. Run: m2sdr_sata catalog-init\n");
        return 1;
    }
    return 0;
}

static struct sata_capture_entry *catalog_find(struct sata_catalog *cat, const char *name)
{
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        if (cat->entries[i].used && strcmp(cat->entries[i].name, name) == 0)
            return &cat->entries[i];
    }
    return NULL;
}

static int catalog_first_free(struct sata_catalog *cat)
{
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        if (!cat->entries[i].used)
            return i;
    }
    return -1;
}

static uint64_t catalog_end_sector(const struct sata_capture_entry *e)
{
    return e->sector + (uint64_t)e->nsectors;
}

static bool catalog_regions_overlap(uint64_t a_start, uint64_t a_count,
                                    uint64_t b_start, uint64_t b_count)
{
    uint64_t a_end = a_start + a_count;
    uint64_t b_end = b_start + b_count;

    return a_start < b_end && b_start < a_end;
}

static int catalog_validate_new_region(struct sata_catalog *cat, const char *name,
                                       uint64_t sector, uint32_t nsectors)
{
    if (!catalog_name_valid(name)) {
        fprintf(stderr, "Invalid capture name. Use a short name without spaces or '|'.\n");
        return 1;
    }
    if (catalog_find(cat, name)) {
        fprintf(stderr, "Capture '%s' already exists.\n", name);
        return 1;
    }
    if (nsectors == 0) {
        fprintf(stderr, "Capture sector count must be greater than zero.\n");
        return 1;
    }
    if (sector < SATA_DATA_START) {
        fprintf(stderr, "Capture sector 0x%016" PRIx64 " is before data start 0x%016" PRIx64 ".\n",
            sector, (uint64_t)SATA_DATA_START);
        return 1;
    }
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &cat->entries[i];
        if (!e->used)
            continue;
        if (catalog_regions_overlap(sector, nsectors, e->sector, e->nsectors)) {
            fprintf(stderr, "Capture overlaps '%s' at 0x%016" PRIx64 "..0x%016" PRIx64 ".\n",
                e->name, e->sector, catalog_end_sector(e));
            return 1;
        }
    }
    return 0;
}

static uint64_t catalog_alloc_sector(struct sata_catalog *cat, uint32_t nsectors)
{
    uint64_t sector = SATA_DATA_START;

    for (;;) {
        bool moved = false;
        for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
            const struct sata_capture_entry *e = &cat->entries[i];
            if (!e->used)
                continue;
            if (catalog_regions_overlap(sector, nsectors, e->sector, e->nsectors)) {
                sector = catalog_end_sector(e);
                moved = true;
            }
        }
        if (!moved)
            return sector;
    }
}

static void catalog_entry_print(const struct sata_capture_entry *e)
{
    printf("Name           : %s\n", e->name);
    printf("Sector         : 0x%016" PRIx64 "\n", e->sector);
    printf("Sectors        : %" PRIu32 "\n", e->nsectors);
    printf("Bytes          : %" PRIu64 "\n", e->bytes);
    printf("Sample Rate    : %" PRId64 "\n", e->sample_rate);
    printf("Format         : %s\n", e->format);
    printf("Channel Layout : %s\n", e->channel_layout);
    printf("RX Frequency   : %" PRIu64 "\n", e->rx_freq);
    printf("TX Frequency   : %" PRIu64 "\n", e->tx_freq);
    printf("Bandwidth      : %" PRIu64 "\n", e->bandwidth);
    printf("RX Gain        : %" PRId64 "\n", e->rx_gain);
    printf("TX Attenuation : %" PRId64 "\n", e->tx_att);
    printf("Created        : %" PRIu64 "\n", e->created);
    if (e->notes[0])
        printf("Notes          : %s\n", e->notes);
}

static int catalog_add_entry(struct sata_catalog *cat, const struct sata_capture_entry *entry)
{
    int slot = catalog_first_free(cat);

    if (slot < 0) {
        fprintf(stderr, "SATA catalog is full.\n");
        return 1;
    }
    cat->entries[slot] = *entry;
    cat->entries[slot].used = true;
    return 0;
}

static int do_catalog_init(int timeout_ms)
{
    struct sata_catalog cat;
    struct m2sdr_dev *conn;
    int rc;

    catalog_clear(&cat);
    conn = m2sdr_open_dev();
    sata_require_csrs();
    rc = catalog_save_to_conn(conn, &cat, timeout_ms);
    m2sdr_close_dev(conn);
    if (rc != 0)
        return 1;
    printf("Initialized SATA catalog at sector 0x%016" PRIx64 " (%u sectors).\n",
        (uint64_t)SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS);
    return 0;
}

static int do_catalog_list(int timeout_ms)
{
    struct sata_catalog cat;
    int count = 0;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;

    printf("%-24s %-18s %-10s %-12s %-8s %-8s\n",
        "NAME", "SECTOR", "SECTORS", "RATE", "FORMAT", "CHANS");
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &cat.entries[i];
        if (!e->used)
            continue;
        printf("%-24s 0x%016" PRIx64 " %-10" PRIu32 " %-12" PRId64 " %-8s %-8s\n",
            e->name, e->sector, e->nsectors, e->sample_rate, e->format, e->channel_layout);
        count++;
    }
    if (count == 0)
        printf("(empty)\n");
    return 0;
}

static int do_catalog_show(const char *name, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    catalog_entry_print(e);
    return 0;
}

static int do_catalog_delete(const char *name, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    struct m2sdr_dev *conn;
    int rc;

    conn = m2sdr_open_dev();
    sata_require_csrs();
    rc = catalog_load_from_conn(conn, &cat, timeout_ms);
    if (rc != 0) {
        m2sdr_close_dev(conn);
        return 1;
    }
    if (!cat.initialized) {
        fprintf(stderr, "SATA catalog is not initialized. Run: m2sdr_sata catalog-init\n");
        m2sdr_close_dev(conn);
        return 1;
    }
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        m2sdr_close_dev(conn);
        return 1;
    }
    memset(e, 0, sizeof(*e));
    rc = catalog_save_to_conn(conn, &cat, timeout_ms);
    m2sdr_close_dev(conn);
    if (rc != 0)
        return 1;
    printf("Deleted capture '%s' from catalog. Data sectors were not erased.\n", name);
    return 0;
}

static int do_catalog_fsck(int timeout_ms)
{
    struct sata_catalog cat;
    int errors = 0;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *a = &cat.entries[i];
        if (!a->used)
            continue;
        if (!catalog_name_valid(a->name) || a->nsectors == 0 || a->sector < SATA_DATA_START) {
            fprintf(stderr, "Invalid entry: %s\n", a->name);
            errors++;
        }
        for (int j = i + 1; j < SATA_CATALOG_MAX_ENTRIES; j++) {
            const struct sata_capture_entry *b = &cat.entries[j];
            if (!b->used)
                continue;
            if (strcmp(a->name, b->name) == 0) {
                fprintf(stderr, "Duplicate name: %s\n", a->name);
                errors++;
            }
            if (catalog_regions_overlap(a->sector, a->nsectors, b->sector, b->nsectors)) {
                fprintf(stderr, "Overlap: %s and %s\n", a->name, b->name);
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("SATA catalog OK.\n");
        return 0;
    }
    fprintf(stderr, "SATA catalog has %d error(s).\n", errors);
    return 1;
}

static int do_export_capture(const char *name, const char *path, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    struct m2sdr_dev *conn;
    uint32_t max_sectors = sata_host_buffer_max_sectors();
    uint8_t *buf;
    uint32_t done = 0;
    uint64_t remaining;
    bool close_out = false;
    FILE *out;
    int rc = 1;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }

    remaining = e->bytes ? e->bytes : (uint64_t)e->nsectors * SATA_SECTOR_BYTES;
    buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    if (!buf) {
        fprintf(stderr, "Failed to allocate export buffer.\n");
        return 1;
    }
    out = open_stdio_or_file(path, "wb", &close_out);
    if (!out)
        goto out_free;

    conn = m2sdr_open_dev();
    sata_require_csrs();
    while (done < e->nsectors && remaining > 0) {
        uint32_t chunk = e->nsectors - done;
        size_t chunk_bytes;
        size_t write_bytes;

        if (chunk > max_sectors)
            chunk = max_sectors;
        chunk_bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        if (sata_read_to_host_buffer_quiet(conn, e->sector + done, chunk, timeout_ms) != 0)
            goto out_close_dev;
        sata_host_buffer_read(conn, buf, chunk_bytes);
        write_bytes = remaining < chunk_bytes ? (size_t)remaining : chunk_bytes;
        if (fwrite(buf, 1, write_bytes, out) != write_bytes) {
            perror(path);
            goto out_close_dev;
        }
        remaining -= write_bytes;
        done += chunk;
    }
    rc = 0;
    printf("Exported '%s' to %s (%" PRIu64 " bytes).\n",
        name, path, e->bytes ? e->bytes : (uint64_t)e->nsectors * SATA_SECTOR_BYTES);

out_close_dev:
    m2sdr_close_dev(conn);
    if (close_out)
        fclose(out);
out_free:
    free(buf);
    return rc;
}

static uint64_t file_size_bytes(const char *path)
{
    struct stat st;

    if (!path || !strcmp(path, "-")) {
        fprintf(stderr, "Named import requires a regular file path, not '-'.\n");
        exit(1);
    }
    if (stat(path, &st) != 0) {
        perror(path);
        exit(1);
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s is not a regular file.\n", path);
        exit(1);
    }
    return (uint64_t)st.st_size;
}

struct named_transfer_options {
    bool have_sector;
    uint64_t sector;
    int64_t sample_rate;
    enum m2sdr_format format;
    enum m2sdr_channel_layout channel_layout;
    uint64_t rx_freq;
    uint64_t tx_freq;
    uint64_t bandwidth;
    int64_t rx_gain;
    int64_t tx_att;
    char notes[SATA_CAPTURE_NOTES_MAX];
};

static void named_transfer_options_init(struct named_transfer_options *opts)
{
    struct m2sdr_config cfg;

    memset(opts, 0, sizeof(*opts));
    m2sdr_config_init(&cfg);
    opts->sample_rate = cfg.sample_rate;
    opts->format = M2SDR_FORMAT_SC16_Q11;
    opts->channel_layout = cfg.channel_layout;
    opts->rx_freq = (uint64_t)cfg.rx_freq;
    opts->tx_freq = (uint64_t)cfg.tx_freq;
    opts->bandwidth = (uint64_t)cfg.bandwidth;
    opts->rx_gain = cfg.rx_gain1;
    opts->tx_att = cfg.tx_att;
}

static int parse_named_option(struct named_transfer_options *opts, int argc, char **argv, int *index)
{
    const char *opt = argv[*index];
    const char *value = NULL;

    if (strncmp(opt, "--", 2) != 0)
        return 0;
    if (*index + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", opt);
        exit(1);
    }
    value = argv[++(*index)];

    if (!strcmp(opt, "--sector")) {
        opts->sector = parse_u64(value);
        opts->have_sector = true;
    } else if (!strcmp(opt, "--sample-rate") || !strcmp(opt, "--samplerate")) {
        opts->sample_rate = parse_i64("sample rate", value);
    } else if (!strcmp(opt, "--format")) {
        if (m2sdr_cli_parse_format(value, &opts->format) != 0) {
            m2sdr_cli_invalid_choice("format", value, "sc16 or sc8");
            exit(1);
        }
    } else if (!strcmp(opt, "--channel-layout") || !strcmp(opt, "--chan")) {
        opts->channel_layout = parse_channel_layout(value);
    } else if (!strcmp(opt, "--rx-freq") || !strcmp(opt, "--rx_freq")) {
        opts->rx_freq = (uint64_t)parse_i64("RX frequency", value);
    } else if (!strcmp(opt, "--tx-freq") || !strcmp(opt, "--tx_freq")) {
        opts->tx_freq = (uint64_t)parse_i64("TX frequency", value);
    } else if (!strcmp(opt, "--bandwidth")) {
        opts->bandwidth = (uint64_t)parse_i64("bandwidth", value);
    } else if (!strcmp(opt, "--rx-gain") || !strcmp(opt, "--rx_gain")) {
        opts->rx_gain = parse_i64("RX gain", value);
    } else if (!strcmp(opt, "--tx-att") || !strcmp(opt, "--tx_att")) {
        opts->tx_att = parse_i64("TX attenuation", value);
    } else if (!strcmp(opt, "--notes")) {
        if (!catalog_text_valid(value, sizeof(opts->notes))) {
            fprintf(stderr, "Notes are too long or contain '|'/newline.\n");
            exit(1);
        }
        catalog_copy(opts->notes, sizeof(opts->notes), value);
    } else {
        fprintf(stderr, "Unknown option: %s\n", opt);
        exit(1);
    }
    return 1;
}

static void catalog_entry_from_options(struct sata_capture_entry *e,
                                       const char *name,
                                       const struct named_transfer_options *opts,
                                       uint64_t sector,
                                       uint32_t nsectors,
                                       uint64_t bytes)
{
    memset(e, 0, sizeof(*e));
    e->used = true;
    catalog_copy(e->name, sizeof(e->name), name);
    e->sector = sector;
    e->nsectors = nsectors;
    e->bytes = bytes;
    e->sample_rate = opts->sample_rate;
    catalog_copy(e->format, sizeof(e->format), format_name(opts->format));
    catalog_copy(e->channel_layout, sizeof(e->channel_layout), channel_layout_name(opts->channel_layout));
    e->rx_freq = opts->rx_freq;
    e->tx_freq = opts->tx_freq;
    e->bandwidth = opts->bandwidth;
    e->rx_gain = opts->rx_gain;
    e->tx_att = opts->tx_att;
    e->created = (uint64_t)time(NULL);
    catalog_copy(e->notes, sizeof(e->notes), opts->notes);
}

static int do_import_capture(const char *name, const char *path, int argc, char **argv,
                             int argi, int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct named_transfer_options opts;
    struct sata_capture_entry entry;
    uint64_t bytes = file_size_bytes(path);
    uint32_t nsectors;
    uint64_t sector;

    if (bytes == 0) {
        fprintf(stderr, "%s is empty.\n", path);
        return 1;
    }
    nsectors = (uint32_t)((bytes + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
    if ((uint64_t)nsectors * SATA_SECTOR_BYTES < bytes) {
        fprintf(stderr, "Import file is too large.\n");
        return 1;
    }

    named_transfer_options_init(&opts);
    while (argi < argc) {
        if (!parse_named_option(&opts, argc, argv, &argi)) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    if (catalog_load(&cat, timeout_ms) != 0)
        return 1;
    if (!cat.initialized)
        catalog_clear(&cat);
    sector = opts.have_sector ? opts.sector : catalog_alloc_sector(&cat, nsectors);
    if (catalog_validate_new_region(&cat, name, sector, nsectors) != 0)
        return 1;
    if (dry_run) {
        printf("import dry-run: name=%s path=%s sector=0x%016" PRIx64
               " nsectors=%" PRIu32 " bytes=%" PRIu64 "\n",
            name, path, sector, nsectors, bytes);
        return 0;
    }

    if (do_write_file(path, sector, nsectors, true, timeout_ms) != 0)
        return 1;
    catalog_entry_from_options(&entry, name, &opts, sector, nsectors, bytes);
    if (catalog_add_entry(&cat, &entry) != 0)
        return 1;
    if (catalog_save(&cat, timeout_ms) != 0)
        return 1;
    printf("Imported '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
        name, sector, nsectors);
    return 0;
}


#else

static int do_read_file(uint64_t src_sector, uint32_t nsectors, const char *path, int timeout_ms)
{
    (void)src_sector; (void)nsectors; (void)path; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

static int do_write_file(const char *path, uint64_t dst_sector, uint32_t nsectors,
                         bool limit_sectors, int timeout_ms)
{
    (void)path; (void)dst_sector; (void)nsectors; (void)limit_sectors; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

static int do_write_pattern(uint64_t dst_sector, uint32_t nsectors,
                            enum sata_pattern_kind pattern, int timeout_ms)
{
    (void)dst_sector; (void)nsectors; (void)pattern; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

static int do_verify_pattern(uint64_t src_sector, uint32_t nsectors,
                             enum sata_pattern_kind pattern, int timeout_ms)
{
    (void)src_sector; (void)nsectors; (void)pattern; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

#endif

static int do_record(uint64_t dst_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;
    int txsrc = (int)m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);

    /* Normal path: RX -> demux -> SATA_RX_STREAMER. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        txsrc,
        RXDST_SATA
    );

    sata_rx_program(conn, dst_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("record", UINT64_MAX, dst_sector, nsectors, txsrc, RXDST_SATA, 0, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }
    sata_rx_start(conn);

    enum sata_wait_result rc =
        wait_done("SATA_RX(record)", sata_rx_done, sata_rx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return rc == SATA_WAIT_OK ? 0 : 1;
}

static int do_record_start(uint64_t dst_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int txsrc = (int)m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);

    sata_require_csrs();
    txrx_loopback_set(conn, 0);
    crossbar_set(conn, txsrc, RXDST_SATA);
    sata_rx_program(conn, dst_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("record-start", UINT64_MAX, dst_sector, nsectors, txsrc, RXDST_SATA, 0, timeout_ms);
        m2sdr_close_dev(conn);
        return 0;
    }
    sata_rx_start(conn);
    printf("SATA_RX(record): started sector=0x%016" PRIx64 " nsectors=%" PRIu32 "\n",
        dst_sector, nsectors);
    m2sdr_close_dev(conn);
    return 0;
}

static int do_play(uint64_t src_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;
    int rxdst = (int)m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);

    /* Normal path: SATA_TX_STREAMER -> mux -> TX. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        TXSRC_SATA,
        rxdst
    );

    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("play", src_sector, UINT64_MAX, nsectors, TXSRC_SATA, rxdst, 0, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }
    sata_tx_start(conn);

    enum sata_wait_result rc =
        wait_done("SATA_TX(play)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return rc == SATA_WAIT_OK ? 0 : 1;
}

static int do_play_start(uint64_t src_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rxdst = (int)m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);

    sata_require_csrs();
    txrx_loopback_set(conn, 0);
    crossbar_set(conn, TXSRC_SATA, rxdst);
    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("play-start", src_sector, UINT64_MAX, nsectors, TXSRC_SATA, rxdst, 0, timeout_ms);
        m2sdr_close_dev(conn);
        return 0;
    }
    sata_tx_start(conn);
    printf("SATA_TX(play): started sector=0x%016" PRIx64 " nsectors=%" PRIu32 "\n",
        src_sector, nsectors);
    m2sdr_close_dev(conn);
    return 0;
}

static int do_replay(uint64_t src_sector, uint32_t nsectors, const char *dst_s, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;

    int rxdst = parse_rxdst(dst_s);

    /* SATA -> TX -> loopback -> RX destination. */
    crossbar_set(conn, TXSRC_SATA, rxdst);
    txrx_loopback_set(conn, 1);

    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("replay", src_sector, UINT64_MAX, nsectors, TXSRC_SATA, rxdst, 1, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }
    sata_tx_start(conn);

    enum sata_wait_result rc =
        wait_done("SATA_TX(replay)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return rc == SATA_WAIT_OK ? 0 : 1;
}

static int do_copy(uint64_t src_sector, uint64_t dst_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;
    enum sata_wait_result tx_rc = SATA_WAIT_OK;
    enum sata_wait_result rx_rc = SATA_WAIT_OK;
    int rc = 0;

    /* SSD -> SSD:
     * SATA_TX_STREAMER -> TX -> loopback -> RX -> SATA_RX_STREAMER.
     *
     * Sequence one sector at a time. The RX streamer stages one full sector
     * before writing it, and issuing the next SATA read before that write has
     * completed can deadlock the shared SATA core during disk-to-disk copies.
     */
    crossbar_set(conn, TXSRC_SATA, RXDST_SATA);
    txrx_loopback_set(conn, 1);

    if (dry_run) {
        print_planned_transfer("copy", src_sector, dst_sector, nsectors, TXSRC_SATA, RXDST_SATA, 1, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }

    for (uint32_t i = 0; i < nsectors; i++) {
        sata_rx_program(conn, dst_sector + i, 1);
        sata_tx_program(conn, src_sector + i, 1);

        /* Start RX first so the looped-back stream always has a sink. */
        sata_rx_start(conn);
        msleep(5);
        sata_tx_start(conn);

        tx_rc = wait_done_quiet("SATA_TX(copy-src)", sata_tx_done, sata_tx_error, conn, timeout_ms);
        if (tx_rc == SATA_WAIT_OK)
            rx_rc = wait_done_quiet("SATA_RX(copy-dst)", sata_rx_done, sata_rx_error, conn, timeout_ms);

        if (tx_rc != SATA_WAIT_OK || rx_rc != SATA_WAIT_OK) {
            sata_streamers_reset(conn, true, true);
            rc = 1;
            break;
        }

        if (nsectors > 1 && (((i + 1) % 64) == 0 || (i + 1) == nsectors))
            printf("SATA_COPY(copy): copied %" PRIu32 "/%" PRIu32 " sectors\n", i + 1, nsectors);
    }

    if (rc == 0)
        printf("SATA_COPY(copy): done\n");

    sata_operation_finish(&op);
    return rc;
}

static int parse_stream_selector(const char *label, const char *text, bool *rx, bool *tx)
{
    if (!text || !rx || !tx)
        return -1;

    *rx = false;
    *tx = false;
    if (!strcmp(text, "rx")) {
        *rx = true;
        return 0;
    }
    if (!strcmp(text, "tx")) {
        *tx = true;
        return 0;
    }
    if (!strcmp(text, "both")) {
        *rx = true;
        *tx = true;
        return 0;
    }

    m2sdr_cli_invalid_choice(label, text, "rx, tx, or both");
    return -1;
}

static int do_stream_stop(const char *which_s)
{
    bool rx = false;
    bool tx = false;
    struct m2sdr_dev *conn;

    if (parse_stream_selector("stream", which_s, &rx, &tx) != 0)
        return 1;

    conn = m2sdr_open_dev();
    sata_require_csrs();

#ifdef M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR
    sata_streamers_reset(conn, rx, tx);
    printf("SATA streamer reset: %s\n", which_s);
    m2sdr_close_dev(conn);
    return 0;
#else
    fprintf(stderr,
        "SATA streamer stop is not supported by this gateware/software header. "
        "Rebuild and load gateware with CSR_SATA_STREAMER_CONTROL.\n");
    m2sdr_close_dev(conn);
    return 1;
#endif
}

#ifdef SATA_HOST_IO_AVAILABLE

struct capture_options {
    struct named_transfer_options named;
    bool have_seconds;
    double seconds;
    bool have_sectors;
    uint32_t sectors;
};

static void capture_options_init(struct capture_options *opts)
{
    memset(opts, 0, sizeof(*opts));
    named_transfer_options_init(&opts->named);
}

static int parse_capture_option(struct capture_options *opts, int argc, char **argv, int *index)
{
    const char *opt = argv[*index];
    const char *value = NULL;

    if (strcmp(opt, "--seconds") == 0 || strcmp(opt, "--secs") == 0) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", opt);
            exit(1);
        }
        value = argv[++(*index)];
        opts->seconds = parse_double_arg("seconds", value);
        if (opts->seconds <= 0.0) {
            fprintf(stderr, "seconds must be greater than zero.\n");
            exit(1);
        }
        opts->have_seconds = true;
        return 1;
    }
    if (strcmp(opt, "--sectors") == 0) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", opt);
            exit(1);
        }
        value = argv[++(*index)];
        opts->sectors = parse_u32(value);
        if (opts->sectors == 0) {
            fprintf(stderr, "sectors must be greater than zero.\n");
            exit(1);
        }
        opts->have_sectors = true;
        return 1;
    }
    return parse_named_option(&opts->named, argc, argv, index);
}

static int capture_compute_size(const struct capture_options *opts,
                                uint32_t *nsectors,
                                uint64_t *bytes)
{
    if (opts->have_seconds && opts->have_sectors) {
        fprintf(stderr, "Use either --seconds or --sectors, not both.\n");
        return 1;
    }
    if (!opts->have_seconds && !opts->have_sectors) {
        fprintf(stderr, "capture requires --seconds SEC or --sectors N.\n");
        return 1;
    }
    if (opts->have_sectors) {
        *nsectors = opts->sectors;
        *bytes = (uint64_t)opts->sectors * SATA_SECTOR_BYTES;
        return 0;
    }
    {
        unsigned channels = channel_layout_count(opts->named.channel_layout);
        size_t format_bytes = m2sdr_format_size(opts->named.format);
        long double exact_bytes;

        if (opts->named.sample_rate <= 0 || format_bytes == 0) {
            fprintf(stderr, "Invalid capture sample rate or format.\n");
            return 1;
        }
        exact_bytes = (long double)opts->seconds *
                      (long double)opts->named.sample_rate *
                      (long double)channels *
                      (long double)format_bytes;
        if (exact_bytes <= 0.0L || exact_bytes > (long double)UINT64_MAX) {
            fprintf(stderr, "Capture size is out of range.\n");
            return 1;
        }
        {
            long double whole = floorl(exact_bytes);
            long double frac = exact_bytes - whole;
            *bytes = (uint64_t)((frac < 1e-6L) ? whole : ceill(exact_bytes));
        }
        *nsectors = (uint32_t)((*bytes + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
        if (*nsectors == 0 || (uint64_t)*nsectors * SATA_SECTOR_BYTES < *bytes) {
            fprintf(stderr, "Capture sector count is out of range.\n");
            return 1;
        }
    }
    return 0;
}

static int apply_rf_config_from_options(const struct named_transfer_options *opts)
{
    struct m2sdr_config cfg;
    struct m2sdr_dev *dev;
    int rc;

    m2sdr_config_init(&cfg);
    cfg.sample_rate = opts->sample_rate;
    cfg.bandwidth = (int64_t)opts->bandwidth;
    cfg.rx_freq = (int64_t)opts->rx_freq;
    cfg.tx_freq = (int64_t)opts->tx_freq;
    cfg.rx_gain1 = opts->rx_gain;
    cfg.rx_gain2 = opts->rx_gain;
    cfg.tx_att = opts->tx_att;
    cfg.program_rx_gains = true;
    cfg.enable_8bit_mode = (opts->format == M2SDR_FORMAT_SC8_Q7);
    cfg.channel_layout = opts->channel_layout;
    cfg.chan_mode = channel_layout_name(opts->channel_layout);

    dev = m2sdr_open_dev();
    rc = m2sdr_apply_config(dev, &cfg);
    if (rc != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_apply_config failed: %s\n", m2sdr_strerror(rc));
    m2sdr_close_dev(dev);
    return rc == M2SDR_ERR_OK ? 0 : 1;
}

static int catalog_entry_to_options(const struct sata_capture_entry *e,
                                    struct named_transfer_options *opts)
{
    named_transfer_options_init(opts);
    opts->sample_rate = e->sample_rate;
    if (m2sdr_cli_parse_format(e->format, &opts->format) != 0) {
        fprintf(stderr, "Capture '%s' has invalid format '%s'.\n", e->name, e->format);
        return 1;
    }
    opts->channel_layout = parse_channel_layout(e->channel_layout);
    opts->rx_freq = e->rx_freq;
    opts->tx_freq = e->tx_freq;
    opts->bandwidth = e->bandwidth;
    opts->rx_gain = e->rx_gain;
    opts->tx_att = e->tx_att;
    catalog_copy(opts->notes, sizeof(opts->notes), e->notes);
    return 0;
}

static void parse_replay_rf_overrides(struct named_transfer_options *opts,
                                      int argc, char **argv, int argi)
{
    while (argi < argc) {
        if (!parse_named_option(opts, argc, argv, &argi)) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            exit(1);
        }
        argi++;
    }
}

static int do_capture_named(const char *name, int argc, char **argv, int argi,
                            int timeout_ms, bool start_only, bool dry_run)
{
    struct sata_catalog cat;
    struct capture_options opts;
    struct sata_capture_entry entry;
    uint64_t bytes = 0;
    uint32_t nsectors = 0;
    uint64_t sector;

    capture_options_init(&opts);
    while (argi < argc) {
        if (!parse_capture_option(&opts, argc, argv, &argi)) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }
    if (capture_compute_size(&opts, &nsectors, &bytes) != 0)
        return 1;
    if (catalog_load(&cat, timeout_ms) != 0)
        return 1;
    if (!cat.initialized)
        catalog_clear(&cat);
    sector = opts.named.have_sector ? opts.named.sector : catalog_alloc_sector(&cat, nsectors);
    if (catalog_validate_new_region(&cat, name, sector, nsectors) != 0)
        return 1;
    if (dry_run) {
        printf("%s dry-run: name=%s sector=0x%016" PRIx64 " nsectors=%" PRIu32
               " bytes=%" PRIu64 " sample_rate=%" PRId64 " format=%s channels=%s\n",
            start_only ? "capture-start" : "capture",
            name, sector, nsectors, bytes, opts.named.sample_rate,
            format_name(opts.named.format), channel_layout_name(opts.named.channel_layout));
        return 0;
    }

    if (apply_rf_config_from_options(&opts.named) != 0)
        return 1;
    if (start_only) {
        catalog_entry_from_options(&entry, name, &opts.named, sector, nsectors, bytes);
        if (catalog_add_entry(&cat, &entry) != 0)
            return 1;
        if (catalog_save(&cat, timeout_ms) != 0)
            return 1;
        if (do_record_start(sector, nsectors, timeout_ms, false) != 0)
            return 1;
        printf("Started capture '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
            name, sector, nsectors);
        return 0;
    } else {
        if (do_record(sector, nsectors, timeout_ms, false) != 0)
            return 1;
    }

    catalog_entry_from_options(&entry, name, &opts.named, sector, nsectors, bytes);
    if (catalog_add_entry(&cat, &entry) != 0)
        return 1;
    if (catalog_save(&cat, timeout_ms) != 0)
        return 1;
    printf("Recorded capture '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
        name, sector, nsectors);
    return 0;
}

static int do_replay_host_named(const char *name, int argc, char **argv, int argi,
                                int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    const char *dst = NULL;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--dst")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "Missing value for --dst\n");
                return 1;
            }
            dst = argv[++argi];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }
    if (!dst) {
        fprintf(stderr, "replay-host requires --dst pcie|eth.\n");
        return 1;
    }
    if (strcmp(dst, "pcie") != 0 && strcmp(dst, "eth") != 0) {
        m2sdr_cli_invalid_choice("replay-host destination", dst, "pcie or eth");
        return 1;
    }
    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    return do_replay(e->sector, e->nsectors, dst, timeout_ms, dry_run);
}

static int do_replay_rf_named(const char *name, int argc, char **argv, int argi,
                              int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    struct named_transfer_options opts;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    if (catalog_entry_to_options(e, &opts) != 0)
        return 1;
    parse_replay_rf_overrides(&opts, argc, argv, argi);
    if (dry_run)
        return do_play(e->sector, e->nsectors, timeout_ms, true);
    if (apply_rf_config_from_options(&opts) != 0)
        return 1;
    return do_play(e->sector, e->nsectors, timeout_ms, false);
}

#endif /* SATA_HOST_IO_AVAILABLE */

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
    printf("  TX streamer        sector=0x%016" PRIx64 " nsectors=%" PRIu32 " done=%" PRIu32 " error=%" PRIu32 "\n",
        csr_read64(conn, CSR_SATA_TX_STREAMER_SECTOR_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_NSECTORS_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_DONE_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR));
#endif
#ifdef CSR_SATA_RX_STREAMER_BASE
    printf("  RX streamer        sector=0x%016" PRIx64 " nsectors=%" PRIu32 " done=%" PRIu32 " error=%" PRIu32 "\n",
        csr_read64(conn, CSR_SATA_RX_STREAMER_SECTOR_ADDR),
        m2sdr_read32(conn, CSR_SATA_RX_STREAMER_NSECTORS_ADDR),
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
           "  -c, --device-num N             Select the device (default: 0).\n"
           "  -i, --ip ADDR                  Target IP address for Etherbone.\n"
           "  -p, --port PORT                Port number (default: 1234).\n"
           "      --timeout-ms MS            Timeout for streamer operations (default: 30000, -1=infinite).\n"
           "      --dry-run                  Show planned operation without starting the streamer.\n"
           "      --pattern NAME             Pattern for write-pattern/verify-pattern: zero|counter|prbs.\n"
           "      --no-bulk-etherbone        Disable multi-word Etherbone host-buffer access.\n"
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
           "record-start DST_SECTOR NSECTORS\n"
           "    Start RX stream -> SSD and return immediately.\n"
           "\n"
           "play SRC_SECTOR NSECTORS\n"
           "    SSD -> TX stream (SATA_TX_STREAMER).\n"
           "\n"
           "play-start SRC_SECTOR NSECTORS\n"
           "    Start SSD -> TX stream and return immediately.\n"
           "\n"
           "stream-status\n"
           "    Alias for status, useful after nonblocking starts.\n"
           "\n"
           "stream-stop RX|TX|BOTH\n"
           "    Reset the selected SATA streamer(s) when the gateware exposes the stop CSR.\n"
           "\n"
           "replay SRC_SECTOR NSECTORS DST\n"
           "    SSD -> TX -> loopback -> RX destination.\n"
           "    dst: pcie|eth|sata\n"
           "\n"
           "copy SRC_SECTOR DST_SECTOR NSECTORS\n"
           "    SSD -> SSD copy using loopback.\n"
           "\n"
           "read-file SRC_SECTOR NSECTORS FILE|-\n"
           "    Read sectors from SSD through the host staging buffer.\n"
           "\n"
           "write-file FILE|- DST_SECTOR [NSECTORS]\n"
           "    Write sectors to SSD through the host staging buffer.\n"
           "\n"
           "write-pattern DST_SECTOR NSECTORS\n"
           "    Fill sectors with the selected --pattern.\n"
           "\n"
           "verify-pattern SRC_SECTOR NSECTORS\n"
           "    Read sectors and compare against the selected --pattern.\n"
           "\n"
#ifdef SATA_HOST_IO_AVAILABLE
           "etherbone-bench [--iterations N]\n"
           "    Sweep Etherbone host-buffer burst sizes and report write/read throughput.\n"
           "\n"
           "catalog-init\n"
           "    Initialize/reset the named capture catalog.\n"
           "\n"
           "list\n"
           "    List named captures from the SATA catalog.\n"
           "\n"
           "show NAME\n"
           "    Show metadata for one named capture.\n"
           "\n"
           "delete NAME\n"
           "    Remove one capture from the catalog without erasing data sectors.\n"
           "\n"
           "fsck\n"
           "    Check catalog names and sector overlap.\n"
           "\n"
           "capture NAME --seconds SEC|--sectors N [RF options]\n"
           "    Tune RF, record RX stream to SATA, and catalog it.\n"
           "\n"
           "capture-start NAME --seconds SEC|--sectors N [RF options]\n"
           "    Start a named RX-to-SATA capture and return immediately.\n"
           "\n"
           "capture-status\n"
           "    Alias for stream-status.\n"
           "\n"
           "import NAME FILE [metadata options]\n"
           "    Write a host file to SATA and catalog it.\n"
           "\n"
           "export NAME FILE|-\n"
           "    Read a named capture to a host file.\n"
           "\n"
           "replay-host NAME --dst pcie|eth\n"
           "    Replay SATA content through loopback to a normal host RX stream.\n"
           "\n"
           "replay-rf NAME [RF overrides]\n"
           "    Replay SATA content to the RF TX path.\n"
           "\n"
           "Named capture/RF options: --sector, --sample-rate, --format sc16|sc8,\n"
           "    --channel-layout 1t1r|2t2r, --rx-freq, --tx-freq, --bandwidth,\n"
           "    --rx-gain, --tx-att, --notes.\n"
           "\n"
#endif
#else
           "record/play/replay/copy/read-file/write-file/write-pattern/verify-pattern\n"
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

    int timeout_ms = 30000;
    bool dry_run = false;
    const char *pattern_name = "counter";
    m2sdr_cli_device_init(&g_cli_dev);
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "timeout-ms", required_argument, NULL, 'T' },
        { "dry-run", no_argument, NULL, 'n' },
        { "pattern", required_argument, NULL, 'P' },
        { "no-bulk-etherbone", no_argument, NULL, 1000 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, int_handler);
    for (;;) {
        c = getopt_long(argc, argv, "+hd:c:i:p:T:nP:", options, &option_index);
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
        case 'n':
            dry_run = true;
            break;
        case 'P':
            pattern_name = optarg;
            break;
        case 1000:
            g_no_bulk_etherbone = true;
            break;
        default:
            exit(1);
        }
    }

    if (optind >= argc)
        help();

#ifndef CSR_SATA_PHY_BASE
    (void)timeout_ms;
    (void)dry_run;
#endif

    cmd = argv[optind++];

    if (!strcmp(cmd, "status") || !strcmp(cmd, "stream-status") || !strcmp(cmd, "capture-status")) {
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
        return do_record(dst_sector, nsectors, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "record-start")) {
        if (optind + 2 > argc) help();
        uint64_t dst_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
            return 1;
        }
        return do_record_start(dst_sector, nsectors, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "play")) {
        if (optind + 2 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
            return 1;
        }
        return do_play(src_sector, nsectors, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "play-start")) {
        if (optind + 2 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
            return 1;
        }
        return do_play_start(src_sector, nsectors, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "stream-stop")) {
        if (optind + 1 > argc) help();
        return do_stream_stop(argv[optind++]);
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
        return do_replay(src_sector, nsectors, dst, timeout_ms, dry_run);
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
        return do_copy(src_sector, dst_sector, nsectors, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "read-file")) {
        if (optind + 3 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        const char *path    = argv[optind++];
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
            return 1;
        }
        if (dry_run) {
            printf("read-file dry-run: sector=0x%016" PRIx64 " nsectors=%" PRIu32 " path=%s\n",
                src_sector, nsectors, path);
            return 0;
        }
        return do_read_file(src_sector, nsectors, path, timeout_ms);
    }

    if (!strcmp(cmd, "write-file")) {
        if (optind + 2 > argc) help();
        const char *path      = argv[optind++];
        uint64_t dst_sector   = parse_u64(argv[optind++]);
        uint32_t nsectors     = 0;
        bool limit_sectors    = false;
        if (optind < argc) {
            nsectors = parse_u32(argv[optind++]);
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            limit_sectors = true;
        }
        if (dry_run) {
            printf("write-file dry-run: path=%s sector=0x%016" PRIx64,
                path, dst_sector);
            if (limit_sectors)
                printf(" nsectors=%" PRIu32, nsectors);
            printf("\n");
            return 0;
        }
        return do_write_file(path, dst_sector, nsectors, limit_sectors, timeout_ms);
    }

    if (!strcmp(cmd, "write-pattern")) {
        if (optind + 2 > argc) help();
        uint64_t dst_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
            return 1;
        }
        if (dry_run) {
            printf("write-pattern dry-run: sector=0x%016" PRIx64 " nsectors=%" PRIu32 " pattern=%s\n",
                dst_sector, nsectors, pattern_name);
            return 0;
        }
        return do_write_pattern(dst_sector, nsectors, parse_pattern(pattern_name), timeout_ms);
    }

    if (!strcmp(cmd, "verify-pattern")) {
        if (optind + 2 > argc) help();
        uint64_t src_sector = parse_u64(argv[optind++]);
        uint32_t nsectors   = parse_u32(argv[optind++]);
        if (nsectors == 0) {
            m2sdr_cli_error("nsectors must be greater than zero");
            return 1;
        }
        if (dry_run) {
            printf("verify-pattern dry-run: sector=0x%016" PRIx64 " nsectors=%" PRIu32 " pattern=%s\n",
                src_sector, nsectors, pattern_name);
            return 0;
        }
        return do_verify_pattern(src_sector, nsectors, parse_pattern(pattern_name), timeout_ms);
    }

#ifdef SATA_HOST_IO_AVAILABLE
    if (!strcmp(cmd, "etherbone-bench")) {
        return do_etherbone_bench(argc, argv, optind);
    }

    if (!strcmp(cmd, "catalog-init")) {
        return do_catalog_init(timeout_ms);
    }

    if (!strcmp(cmd, "list")) {
        return do_catalog_list(timeout_ms);
    }

    if (!strcmp(cmd, "show")) {
        if (optind + 1 > argc) help();
        return do_catalog_show(argv[optind++], timeout_ms);
    }

    if (!strcmp(cmd, "delete")) {
        if (optind + 1 > argc) help();
        return do_catalog_delete(argv[optind++], timeout_ms);
    }

    if (!strcmp(cmd, "fsck")) {
        return do_catalog_fsck(timeout_ms);
    }

    if (!strcmp(cmd, "import")) {
        if (optind + 2 > argc) help();
        const char *name = argv[optind++];
        const char *path = argv[optind++];
        return do_import_capture(name, path, argc, argv, optind, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "export")) {
        if (optind + 2 > argc) help();
        const char *name = argv[optind++];
        const char *path = argv[optind++];
        return do_export_capture(name, path, timeout_ms);
    }

    if (!strcmp(cmd, "capture")) {
        if (optind + 1 > argc) help();
        const char *name = argv[optind++];
        return do_capture_named(name, argc, argv, optind, timeout_ms, false, dry_run);
    }

    if (!strcmp(cmd, "capture-start")) {
        if (optind + 1 > argc) help();
        const char *name = argv[optind++];
        return do_capture_named(name, argc, argv, optind, timeout_ms, true, dry_run);
    }

    if (!strcmp(cmd, "replay-host")) {
        if (optind + 1 > argc) help();
        const char *name = argv[optind++];
        return do_replay_host_named(name, argc, argv, optind, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "replay-rf")) {
        if (optind + 1 > argc) help();
        const char *name = argv[optind++];
        return do_replay_rf_named(name, argc, argv, optind, timeout_ms, dry_run);
    }
#endif
#else
    if (!strcmp(cmd, "record") || !strcmp(cmd, "record-start") ||
        !strcmp(cmd, "play") || !strcmp(cmd, "play-start") ||
        !strcmp(cmd, "stream-stop") || !strcmp(cmd, "replay") || !strcmp(cmd, "copy") ||
        !strcmp(cmd, "read-file") || !strcmp(cmd, "write-file") ||
        !strcmp(cmd, "write-pattern") || !strcmp(cmd, "verify-pattern")) {
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
