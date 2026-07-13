/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA low-level helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_SATA_LOWLEVEL_H
#define M2SDR_SATA_LOWLEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m2sdr.h"
#include "csr.h"
#include "mem.h"

#define M2SDR_SATA_ETHERBONE_BULK_WORDS  128u
/* Pipelined bulk reads when the Etherbone endpoint is otherwise idle. A
 * single outstanding read keeps catalog traffic cooperative while a live
 * SoapySDR/GQRX client shares the same gateware Etherbone endpoint. */
#define M2SDR_SATA_ETHERBONE_READ_WINDOW        8u
#define M2SDR_SATA_ETHERBONE_READ_WINDOW_SHARED 1u

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

struct sata_route_state {
    bool crossbar_valid;
    bool loopback_valid;
    int txsrc;
    int rxdst;
    int loopback_en;
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

uint32_t m2sdr_read32(struct m2sdr_dev *dev, uint32_t addr);
void     m2sdr_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);
void     csr_write64(struct m2sdr_dev *dev, uint32_t addr, uint64_t val);
uint64_t csr_read64(struct m2sdr_dev *dev, uint32_t addr);

int         parse_txsrc(const char *s);
int         parse_rxdst(const char *s);
const char *txsrc_name(int txsrc);
const char *rxdst_name(int rxdst);
void        crossbar_set(void *conn, int txsrc, int rxdst);
void        header_set_raw(void *conn, int which, int enable, int header_enable);

void sata_wait_sleep(int64_t elapsed_us);
int64_t m2sdr_sata_get_time_us(void);

struct sata_route_state sata_route_state_capture(void *conn);
void sata_route_state_restore(void *conn, const struct sata_route_state *state);

void sata_require_csrs(void);
void txrx_loopback_set(void *conn, int enable);
void sata_rx_program(void *conn, uint64_t sector, uint32_t nsectors);
void sata_tx_program(void *conn, uint64_t sector, uint32_t nsectors);
int  sata_eth_replay_destination_prepare(void *conn);
bool sata_rx_tap_supported(void);
void sata_rx_set_tap(void *conn, bool enable);
bool sata_streamer_stop_supported(bool rx, bool tx);
void sata_streamer_request_stop(void *conn, bool rx, bool tx);
void sata_tx_set_pace(void *conn, uint32_t words_per_second);
void sata_rx_start(void *conn);
void sata_tx_start(void *conn);
int  sata_streamers_reset(void *conn, bool rx, bool tx);

uint32_t sata_rx_done(void *conn);
uint32_t sata_tx_done(void *conn);
uint32_t sata_rx_error(void *conn);
uint32_t sata_tx_error(void *conn);
bool     sata_rx_progress_supported(void);
uint32_t sata_rx_progress(void *conn);
bool     sata_tx_progress_supported(void);
uint32_t sata_tx_progress(void *conn);

void m2sdr_sata_set_no_bulk_etherbone(bool no_bulk);
enum sata_pattern_kind parse_pattern(const char *text);

#ifdef SATA_HOST_IO_AVAILABLE
uint32_t sata_sector2mem_done(void *conn);
uint32_t sata_sector2mem_error(void *conn);
uint32_t sata_mem2sector_done(void *conn);
uint32_t sata_mem2sector_error(void *conn);
void     sata_sector2mem_program(void *conn, uint64_t sector, uint32_t nsectors, uint64_t base);
void     sata_mem2sector_program(void *conn, uint64_t sector, uint32_t nsectors, uint64_t base);

uint32_t sata_host_buffer_max_sectors(void);
void     sata_host_buffer_write(struct m2sdr_dev *conn, const uint8_t *buf, size_t bytes);
void     sata_host_buffer_read(struct m2sdr_dev *conn, uint8_t *buf, size_t bytes);
uint32_t sata_host_buffer_bulk_words(struct m2sdr_dev *conn);

void     etherbone_fill_test_words(uint32_t *words, uint32_t count);
bool     etherbone_probe_burst_words(struct m2sdr_dev *conn, uint32_t count, bool verbose);

void     fill_pattern(uint8_t *buf, size_t len, uint64_t start_byte, enum sata_pattern_kind pattern);
bool     check_pattern(const uint8_t *buf, size_t len, uint64_t start_byte,
                       enum sata_pattern_kind pattern, uint64_t *bad_offset,
                       uint32_t *expected, uint32_t *actual);
#endif

#endif /* M2SDR_SATA_LOWLEVEL_H */
