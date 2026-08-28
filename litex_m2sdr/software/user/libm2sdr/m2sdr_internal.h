/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_INTERNAL_H
#define M2SDR_INTERNAL_H

/* Includes */
/*----------*/

#include <stdint.h>
#include <pthread.h>

#include "m2sdr.h"
#include "liblitepcie.h"
#include "etherbone.h"
#include "liteeth_udp.h"

struct ad9361_rf_phy;

enum m2sdr_transport {
    M2SDR_TRANSPORT_LITEPCIE = 0,
    M2SDR_TRANSPORT_LITEETH  = 1,
};

struct m2sdr_backend_ops {
    int (*readl)(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
    int (*writel)(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);
    int (*readl_bulk)(struct m2sdr_dev *dev, uint32_t addr, uint32_t *vals, size_t count);
    int (*writel_bulk)(struct m2sdr_dev *dev, uint32_t addr, const uint32_t *vals, size_t count);
};

/* Internal device object shared by the transport, stream, and RF layers. */
struct m2sdr_dev {
    enum m2sdr_transport transport;
    const struct m2sdr_backend_ops *ops;

    int fd;
    char device_path[M2SDR_DEVICE_STR_MAX];
    struct litepcie_dma_ctrl rx_dma;
    struct litepcie_dma_ctrl tx_dma;

    struct eb_connection *eb;
    char eth_ip[64];
    uint16_t eth_port;
    int udp_inited;
    struct liteeth_udp_ctrl udp;
    struct m2sdr_liteeth_rx_stream_config liteeth_rx_config;
    int liteeth_rx_config_valid;
    int liteeth_rx_timeout_recovery_armed;
    int liteeth_rx_timeout_recovery_disabled;

    int rx_configured;
    int tx_configured;
    int rx_header_enable;
    int rx_strip_header;
    int tx_header_enable;
    enum m2sdr_format rx_format;
    enum m2sdr_format tx_format;
    uint8_t zero_copy;
    unsigned rx_buffer_size;
    unsigned tx_buffer_size;
    unsigned rx_timeout_ms;
    unsigned tx_timeout_ms;
    int64_t rx_user_count;
    int64_t rx_release_count;
    int64_t tx_user_count;
    int64_t tx_submit_count;
    /* Zero-copy TX fill lead ahead of the LIVE reader cursor, in buffers. 0 = legacy
     * full-ring fill (max throughput/latency); small values (e.g. 2-3) hold the host a tight
     * lead ahead of the free-running reader for low-latency timed TX. See m2sdr_set_tx_lead_buffers. */
    int tx_lead_buffers;
    /* Low-latency zero-copy RX wait via MONITORX/MWAITX. Lazy-init at first RX wait from CPUID
     * support + the M2SDR_RX_WAIT=poll env (rx_wait_initialized guards the calloc-zeroed state). */
    int rx_wait_initialized;
    int rx_wait_mwaitx;                  /* 0 = poll(), 1 = mwaitx (valid once initialized) */
    uint32_t rx_mwaitx_timeout_cycles;   /* MWAITX safety-net timeout in TSC cycles */
    uint64_t pcie_rx_overflow_events;
    uint64_t pcie_rx_overflow_buffers;
    uint64_t pcie_tx_underflow_events;
    uint64_t pcie_tx_underflow_buffers;
    struct ad9361_rf_phy *ad9361_phy;
    /* Per-device snapshot of the AD9361_InitParam used at RF bring-up.
     * The header-defined default_init_param template is per translation
     * unit, so it can neither be shared across files nor across devices;
     * layout switches re-init from this stored copy instead. */
    void *rf_init_param;
    struct m2sdr_config rf_last_config;
    int rf_last_config_valid;
    enum m2sdr_channel_layout rf_channel_layout;
    int rf_channel_layout_valid;
    int rf_oversample_enabled;
    /* Last verified-good AD9361 RX clock delay (REG 0x006 high nibble) chosen by the
     * 2R2T RX-lane deskew; 0 = none yet. The per-lane pair-mismatch metric is blind
     * to some whole-UI capture shifts, so a clock delay can look clean per lane and
     * still fail the sequence-level PRBS verify -- the bring-up rotates through the
     * candidates across verify retries and records the one that verified, so later
     * re-deskews (e.g. after the TX-framing check) reuse it instead of re-rolling. */
    uint8_t rf_deskew_clk;
    /* Last verified-good RX frame-slot rotation (PHY_CONTROL rx_frame_offset);
     * the bring-up's probe order starts here. */
    uint8_t rf_rx_frame_offset;

    /* Serializes register transactions on the shared Etherbone connection;
     * PCIe register access is a single atomic syscall and bypasses it. */
    pthread_mutex_t reg_lock;
};

extern const struct m2sdr_backend_ops m2sdr_litepcie_backend_ops;
extern const struct m2sdr_backend_ops m2sdr_liteeth_backend_ops;

void m2sdr_stream_cleanup(struct m2sdr_dev *dev);

/* MONITORX/MWAITX helpers (m2sdr_mwaitx.c) for the low-latency RX wait. */
int m2sdr_mwaitx_supported(void);
void m2sdr_monitorx(const void *addr);
void m2sdr_mwaitx(unsigned int extensions, unsigned int hints, unsigned int clocks);
uint64_t m2sdr_tsc_hz(void);

int m2sdr_log_is_enabled(void);
void m2sdr_log_printf(const char *fmt, ...);

int m2sdr_test_parse_identifier(const char *id, uint16_t *port_out);

#endif /* M2SDR_INTERNAL_H */
