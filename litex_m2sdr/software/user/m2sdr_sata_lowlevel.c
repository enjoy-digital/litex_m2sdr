/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA low-level helpers.
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
#include <stdint.h>
#include <time.h>

#include "m2sdr_sata_lowlevel.h"
#include "m2sdr_cli.h"
#include "etherbone.h"

static bool no_bulk_etherbone = false;

void m2sdr_sata_set_no_bulk_etherbone(bool no_bulk)
{
    no_bulk_etherbone = no_bulk;
}

/* CSR Access ---------------------------------------------------------------- */

uint32_t m2sdr_read32(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t val = 0;
    if (m2sdr_reg_read(dev, addr, &val) != 0) {
        fprintf(stderr, "CSR read failed @0x%08x\n", addr);
        exit(1);
    }
    return val;
}

void m2sdr_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (m2sdr_reg_write(dev, addr, val) != 0) {
        fprintf(stderr, "CSR write failed @0x%08x\n", addr);
        exit(1);
    }
}

/* 64-bit CSR access (LiteX ordering: upper @ base+0, lower @ base+4). */
void csr_write64(struct m2sdr_dev *dev, uint32_t addr, uint64_t val)
{
    m2sdr_reg_write(dev, addr + 0, (uint32_t)(val >> 32));
    m2sdr_reg_write(dev, addr + 4, (uint32_t)(val >>  0));
}

uint64_t csr_read64(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t upper = 0;
    uint32_t lower = 0;

    m2sdr_reg_read(dev, addr + 0, &upper);
    m2sdr_reg_read(dev, addr + 4, &lower);
    return ((uint64_t)upper << 32) | (uint64_t)lower;
}

/* Crossbar Routing ---------------------------------------------------------- */

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

int parse_txsrc(const char *s)
{
    return parse_route_choice("TX source", s);
}

int parse_rxdst(const char *s)
{
    return parse_route_choice("RX destination", s);
}

const char *txsrc_name(int txsrc)
{
    return route_choice_name(txsrc);
}

const char *rxdst_name(int rxdst)
{
    return route_choice_name(rxdst);
}

void crossbar_set(void *conn, int txsrc, int rxdst)
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

/* Timing Helpers ------------------------------------------------------------ */

static void msleep(unsigned ms)
{
    usleep(ms * 1000);
}

void sata_wait_sleep(int64_t elapsed_us)
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

int64_t m2sdr_sata_get_time_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Optional Header Raw Control ---------------------------------------------- */

void header_set_raw(void *conn, int which, int enable, int header_enable)
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

/* SATA Control -------------------------------------------------------------- */

#ifdef CSR_SATA_PHY_BASE

struct sata_route_state sata_route_state_capture(void *conn)
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

void sata_route_state_restore(void *conn, const struct sata_route_state *state)
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

void sata_require_csrs(void)
{
#if !defined(CSR_SATA_RX_STREAMER_BASE) || !defined(CSR_SATA_TX_STREAMER_BASE) || !defined(CSR_TXRX_LOOPBACK_BASE)
    fprintf(stderr, "SATA blocks not fully present in this gateware.\n");
    exit(1);
#endif
}

void txrx_loopback_set(void *conn, int enable)
{
#ifdef CSR_TXRX_LOOPBACK_BASE
    uint32_t v = 0;

    v |= (enable ? 1u : 0u) << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET;
    m2sdr_write32(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR, v);
#else
    (void)conn; (void)enable;
    fprintf(stderr, "TX/RX loopback CSR not present in this gateware.\n");
    exit(1);
#endif
}

void sata_rx_program(void *conn, uint64_t sector, uint32_t nsectors)
{
    csr_write64(conn, CSR_SATA_RX_STREAMER_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_RX_STREAMER_NSECTORS_ADDR, nsectors);
}

void sata_tx_program(void *conn, uint64_t sector, uint32_t nsectors)
{
    csr_write64(conn, CSR_SATA_TX_STREAMER_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_TX_STREAMER_NSECTORS_ADDR, nsectors);
}

void sata_rx_start(void *conn)
{
    m2sdr_write32(conn, CSR_SATA_RX_STREAMER_START_ADDR, 1);
}

void sata_tx_start(void *conn)
{
    m2sdr_write32(conn, CSR_SATA_TX_STREAMER_START_ADDR, 1);
}

int sata_streamers_reset(void *conn, bool rx, bool tx)
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

uint32_t sata_rx_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_RX_STREAMER_DONE_ADDR);
}

uint32_t sata_tx_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_TX_STREAMER_DONE_ADDR);
}

uint32_t sata_rx_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_RX_STREAMER_ERROR_ADDR);
}

uint32_t sata_tx_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR);
}

/* Pattern Helpers ----------------------------------------------------------- */

enum sata_pattern_kind parse_pattern(const char *text)
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

uint32_t sata_sector2mem_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_SECTOR2MEM_DONE_ADDR);
}

uint32_t sata_sector2mem_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_SECTOR2MEM_ERROR_ADDR);
}

uint32_t sata_mem2sector_done(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_MEM2SECTOR_DONE_ADDR);
}

uint32_t sata_mem2sector_error(void *conn)
{
    return m2sdr_read32(conn, CSR_SATA_MEM2SECTOR_ERROR_ADDR);
}

void sata_sector2mem_program(void *conn, uint64_t sector, uint32_t nsectors, uint64_t base)
{
    csr_write64(conn, CSR_SATA_SECTOR2MEM_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_SECTOR2MEM_NSECTORS_ADDR, nsectors);
    csr_write64(conn, CSR_SATA_SECTOR2MEM_BASE_ADDR, base);
}

void sata_mem2sector_program(void *conn, uint64_t sector, uint32_t nsectors, uint64_t base)
{
    csr_write64(conn, CSR_SATA_MEM2SECTOR_SECTOR_ADDR, sector);
    m2sdr_write32(conn, CSR_SATA_MEM2SECTOR_NSECTORS_ADDR, nsectors);
    csr_write64(conn, CSR_SATA_MEM2SECTOR_BASE_ADDR, base);
}

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

void fill_pattern(uint8_t *buf, size_t len, uint64_t start_byte,
                  enum sata_pattern_kind pattern)
{
    uint64_t word_index = start_byte / 4u;

    for (size_t off = 0; off < len; off += 4)
        put_le32(&buf[off], pattern_word(pattern, word_index++));
}

bool check_pattern(const uint8_t *buf, size_t len, uint64_t start_byte,
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

/* Host Buffer / Etherbone Helpers ------------------------------------------ */

uint32_t sata_host_buffer_max_sectors(void)
{
    return (uint32_t)(SATA_HOST_BUFFER_SIZE / SATA_SECTOR_BYTES);
}

static bool etherbone_is_liteeth(struct m2sdr_dev *conn)
{
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;

    return m2sdr_get_transport(conn, &transport) == M2SDR_ERR_OK &&
           transport == M2SDR_TRANSPORT_KIND_LITEETH;
}

void etherbone_fill_test_words(uint32_t *words, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        words[i] = 0x5a000000u ^ (count << 12) ^ (i * 0x01020408u) ^ i;
}

bool etherbone_probe_burst_words(struct m2sdr_dev *conn, uint32_t count, bool verbose)
{
    uint32_t tx[M2SDR_SATA_ETHERBONE_BULK_WORDS];
    uint32_t rx[M2SDR_SATA_ETHERBONE_BULK_WORDS];

    if (count == 0 || count > M2SDR_SATA_ETHERBONE_BULK_WORDS ||
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

uint32_t sata_host_buffer_bulk_words(struct m2sdr_dev *conn)
{
    static bool probed = false;
    static uint32_t cached_words = 1;

    if (no_bulk_etherbone)
        return 1;
    if (probed)
        return cached_words;
    if (!etherbone_is_liteeth(conn)) {
        cached_words = M2SDR_SATA_ETHERBONE_BULK_WORDS;
        probed = true;
        return cached_words;
    }

    probed = true;
    if (etherbone_probe_burst_words(conn, M2SDR_SATA_ETHERBONE_BULK_WORDS, true)) {
        cached_words = M2SDR_SATA_ETHERBONE_BULK_WORDS;
        fprintf(stderr, "Etherbone bulk burst: %" PRIu32 " words\n", cached_words);
        return cached_words;
    }

    cached_words = 1;
    fprintf(stderr, "Etherbone bulk burst probe failed; using legacy single-word access.\n");
    return cached_words;
}

void sata_host_buffer_write(struct m2sdr_dev *conn, const uint8_t *buf, size_t bytes)
{
    uint32_t burst = sata_host_buffer_bulk_words(conn);
    uint32_t words[M2SDR_SATA_ETHERBONE_BULK_WORDS];

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

void sata_host_buffer_read(struct m2sdr_dev *conn, uint8_t *buf, size_t bytes)
{
    uint32_t burst = sata_host_buffer_bulk_words(conn);
    uint32_t words[M2SDR_SATA_ETHERBONE_BULK_WORDS];

    if (burst > 1 && etherbone_is_liteeth(conn) && (bytes % sizeof(uint32_t)) == 0) {
        struct eb_connection *eb = m2sdr_get_eb_handle(conn);
        size_t word_count = bytes / sizeof(uint32_t);
        uint32_t *pipeline_words = malloc(word_count * sizeof(uint32_t));

        if (eb && pipeline_words) {
            int rc = eb_read32_bulk_pipeline_checked(eb, SATA_HOST_BUFFER_BASE,
                pipeline_words, word_count, burst, M2SDR_SATA_ETHERBONE_READ_WINDOW);

            if (rc == EB_ERR_OK) {
                for (size_t i = 0; i < word_count; i++)
                    put_le32(&buf[4 * i], pipeline_words[i]);
                free(pipeline_words);
                return;
            }
            fprintf(stderr, "Host buffer pipelined read failed; using stop-and-wait reads.\n");
        }
        free(pipeline_words);
    }

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

#endif /* SATA_HOST_IO_AVAILABLE */

#endif /* CSR_SATA_PHY_BASE */
