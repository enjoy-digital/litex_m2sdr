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

#include "liblitepcie.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "csr.h"
#include "mem.h"

/* Connection options -------------------------------------------------------- */

static struct m2sdr_cli_device g_cli_dev;
static sig_atomic_t keep_running = 1;

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

static int64_t m2sdr_sata_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static enum sata_wait_result wait_done(const char *name,
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
        if (!keep_running) {
            fprintf(stderr, "%s: interrupted\n", name);
            return SATA_WAIT_INTERRUPTED;
        }
        uint32_t done = done_fn(conn);
        uint32_t err  = err_fn(conn);
        if (done) {
            if (err) printf("%s: done (error=1)\n", name);
            else     printf("%s: done\n", name);
            return SATA_WAIT_OK;
        }
        if (timeout_ms >= 0 && elapsed >= timeout_ms) {
            fprintf(stderr, "%s: timeout\n", name);
            return SATA_WAIT_TIMEOUT;
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

static void sata_host_buffer_write(void *conn, const uint8_t *buf, size_t bytes)
{
    for (size_t off = 0; off < bytes; off += 4)
        m2sdr_write32(conn, SATA_HOST_BUFFER_BASE + (uint32_t)off, get_le32(&buf[off]));
}

static void sata_host_buffer_read(void *conn, uint8_t *buf, size_t bytes)
{
    for (size_t off = 0; off < bytes; off += 4)
        put_le32(&buf[off], m2sdr_read32(conn, SATA_HOST_BUFFER_BASE + (uint32_t)off));
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

static int sata_read_to_host_buffer(void *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
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

    /* SSD -> SSD:
     * SATA_TX_STREAMER -> TX -> loopback -> RX -> SATA_RX_STREAMER.
     */
    crossbar_set(conn, TXSRC_SATA, RXDST_SATA);
    txrx_loopback_set(conn, 1);

    /* Start RX first. */
    sata_rx_program(conn, dst_sector, nsectors);
    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("copy", src_sector, dst_sector, nsectors, TXSRC_SATA, RXDST_SATA, 1, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }

    sata_rx_start(conn);
    msleep(5);
    sata_tx_start(conn);

    enum sata_wait_result tx_rc =
        wait_done("SATA_TX(copy-src)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    enum sata_wait_result rx_rc = SATA_WAIT_OK;
    if (tx_rc == SATA_WAIT_OK)
        rx_rc = wait_done("SATA_RX(copy-dst)", sata_rx_done, sata_rx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return (tx_rc == SATA_WAIT_OK && rx_rc == SATA_WAIT_OK) ? 0 : 1;
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
    uint32_t control = 0;
    struct m2sdr_dev *conn;

    if (parse_stream_selector("stream", which_s, &rx, &tx) != 0)
        return 1;

    conn = m2sdr_open_dev();
    sata_require_csrs();

#ifdef M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR
    if (rx)
        control |= 1u << M2SDR_CSR_SATA_STREAMER_CONTROL_RX_RESET_OFFSET;
    if (tx)
        control |= 1u << M2SDR_CSR_SATA_STREAMER_CONTROL_TX_RESET_OFFSET;
    m2sdr_write32(conn, M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR, control);
    printf("SATA streamer reset: %s\n", which_s);
    m2sdr_close_dev(conn);
    return 0;
#else
    (void)control;
    fprintf(stderr,
        "SATA streamer stop is not supported by this gateware/software header. "
        "Rebuild and load gateware with CSR_SATA_STREAMER_CONTROL.\n");
    m2sdr_close_dev(conn);
    return 1;
#endif
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
           "      --timeout-ms MS            Timeout for streamer operations (default: 10000, -1=infinite).\n"
           "      --dry-run                  Show planned operation without starting the streamer.\n"
           "      --pattern NAME             Pattern for write-pattern/verify-pattern: zero|counter|prbs.\n"
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

    int timeout_ms = 10000;
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
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, int_handler);
    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:p:T:nP:", options, &option_index);
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

    if (!strcmp(cmd, "status") || !strcmp(cmd, "stream-status")) {
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
