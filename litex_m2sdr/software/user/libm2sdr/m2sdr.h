/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR public C API (BladeRF-style)
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#ifndef M2SDR_H
#define M2SDR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "csr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M2SDR_DEVICE_STR_MAX 256
#define M2SDR_SERIAL_MAX     32
#define M2SDR_IDENT_MAX      256

/* Error codes (0 on success, negative on failure) */
enum m2sdr_error {
    M2SDR_ERR_OK         =  0,
    M2SDR_ERR_UNEXPECTED = -1,
    M2SDR_ERR_INVAL      = -2,
    M2SDR_ERR_IO         = -3,
    M2SDR_ERR_TIMEOUT    = -4,
    M2SDR_ERR_NO_MEM     = -5,
    M2SDR_ERR_UNSUPPORTED= -6,
};

#define M2SDR_API_VERSION 0x00020000
#define M2SDR_ABI_VERSION 0x00020000
#define M2SDR_VERSION_STRING "0.2.0"

struct m2sdr_version {
    uint32_t api;
    uint32_t abi;
    const char *version_str;
};

const char *m2sdr_strerror(int err);
void m2sdr_get_version(struct m2sdr_version *ver);

/* Direction/module */
enum m2sdr_module {
    M2SDR_RX = 0,
    M2SDR_TX = 1,
};

/* Sample format (subset, extend as needed) */
enum m2sdr_format {
    /* 16-bit I/Q interleaved (SC16 Q11 style) */
    M2SDR_FORMAT_SC16_Q11 = 0,
    /* 8-bit I/Q interleaved (SC8 Q7 style) */
    M2SDR_FORMAT_SC8_Q7  = 1,
};

struct m2sdr_metadata {
    uint64_t timestamp;
    uint32_t flags;
};

#define M2SDR_META_FLAG_HAS_TIME (1u << 0)

struct m2sdr_sync_params {
    enum m2sdr_module module;
    enum m2sdr_format format;
    unsigned num_buffers;
    unsigned buffer_size;
    unsigned num_transfers;
    unsigned timeout_ms;
    bool zero_copy;
    bool rx_header_enable;
    bool rx_strip_header;
    bool tx_header_enable;
};

struct m2sdr_devinfo {
    char serial[M2SDR_SERIAL_MAX];
    char identification[M2SDR_IDENT_MAX];
    char path[M2SDR_DEVICE_STR_MAX];
    char transport[16];
};

struct m2sdr_capabilities {
    uint32_t api_version;
    uint32_t features;
    uint32_t board_info;
    uint32_t pcie_config;
    uint32_t eth_config;
    uint32_t sata_config;
};

struct m2sdr_clock_info {
    uint64_t refclk_hz;
    uint64_t sysclk_hz;
};

enum m2sdr_feature_flag {
#ifdef CSR_CAPABILITY_FEATURES_PCIE_OFFSET
    M2SDR_FEATURE_PCIE = 1u << CSR_CAPABILITY_FEATURES_PCIE_OFFSET,
#else
    M2SDR_FEATURE_PCIE = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_ETH_OFFSET
    M2SDR_FEATURE_ETH  = 1u << CSR_CAPABILITY_FEATURES_ETH_OFFSET,
#else
    M2SDR_FEATURE_ETH  = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_SATA_OFFSET
    M2SDR_FEATURE_SATA = 1u << CSR_CAPABILITY_FEATURES_SATA_OFFSET,
#else
    M2SDR_FEATURE_SATA = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_GPIO_OFFSET
    M2SDR_FEATURE_GPIO = 1u << CSR_CAPABILITY_FEATURES_GPIO_OFFSET,
#else
    M2SDR_FEATURE_GPIO = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_WR_OFFSET
    M2SDR_FEATURE_WR   = 1u << CSR_CAPABILITY_FEATURES_WR_OFFSET,
#else
    M2SDR_FEATURE_WR   = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_JTAGBONE_OFFSET
    M2SDR_FEATURE_JTAGBONE = 1u << CSR_CAPABILITY_FEATURES_JTAGBONE_OFFSET,
#else
    M2SDR_FEATURE_JTAGBONE = 0,
#endif
};

enum m2sdr_feature_mask {
#ifdef CSR_CAPABILITY_FEATURES_PCIE_SIZE
    M2SDR_FEATURE_PCIE_MASK = ((1u << CSR_CAPABILITY_FEATURES_PCIE_SIZE) - 1u) << CSR_CAPABILITY_FEATURES_PCIE_OFFSET,
#else
    M2SDR_FEATURE_PCIE_MASK = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_ETH_SIZE
    M2SDR_FEATURE_ETH_MASK  = ((1u << CSR_CAPABILITY_FEATURES_ETH_SIZE)  - 1u) << CSR_CAPABILITY_FEATURES_ETH_OFFSET,
#else
    M2SDR_FEATURE_ETH_MASK  = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_SATA_SIZE
    M2SDR_FEATURE_SATA_MASK = ((1u << CSR_CAPABILITY_FEATURES_SATA_SIZE) - 1u) << CSR_CAPABILITY_FEATURES_SATA_OFFSET,
#else
    M2SDR_FEATURE_SATA_MASK = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_GPIO_SIZE
    M2SDR_FEATURE_GPIO_MASK = ((1u << CSR_CAPABILITY_FEATURES_GPIO_SIZE) - 1u) << CSR_CAPABILITY_FEATURES_GPIO_OFFSET,
#else
    M2SDR_FEATURE_GPIO_MASK = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_WR_SIZE
    M2SDR_FEATURE_WR_MASK   = ((1u << CSR_CAPABILITY_FEATURES_WR_SIZE)   - 1u) << CSR_CAPABILITY_FEATURES_WR_OFFSET,
#else
    M2SDR_FEATURE_WR_MASK   = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_JTAGBONE_SIZE
    M2SDR_FEATURE_JTAGBONE_MASK = ((1u << CSR_CAPABILITY_FEATURES_JTAGBONE_SIZE) - 1u) << CSR_CAPABILITY_FEATURES_JTAGBONE_OFFSET,
#else
    M2SDR_FEATURE_JTAGBONE_MASK = 0,
#endif
};

struct m2sdr_fpga_sensors {
    double temperature_c;
    double vccint_v;
    double vccaux_v;
    double vccbram_v;
};

/* RF configuration (matches existing utilities defaults) */
struct m2sdr_config {
    int64_t sample_rate;
    int64_t bandwidth;
    int64_t refclk_freq;
    int64_t tx_freq;
    int64_t rx_freq;
    int64_t tx_gain;
    int64_t rx_gain1;
    int64_t rx_gain2;
    uint8_t loopback;
    bool    bist_tx_tone;
    bool    bist_rx_tone;
    bool    bist_prbs;
    int32_t bist_tone_freq;
    bool    enable_8bit_mode;
    bool    enable_oversample;
    const char *chan_mode; /* "2r2t", "1r1t", etc. */
    const char *sync_mode; /* "internal" or "external" */
};

struct m2sdr_dev;

/* Device management */
int  m2sdr_open(struct m2sdr_dev **dev, const char *device_identifier);
void m2sdr_close(struct m2sdr_dev *dev);

int  m2sdr_get_device_list(struct m2sdr_devinfo *list, size_t max, size_t *count);

int  m2sdr_get_device_info(struct m2sdr_dev *dev, struct m2sdr_devinfo *info);
int  m2sdr_get_capabilities(struct m2sdr_dev *dev, struct m2sdr_capabilities *caps);
int  m2sdr_get_identifier(struct m2sdr_dev *dev, char *buf, size_t len);
int  m2sdr_get_fpga_git_hash(struct m2sdr_dev *dev, uint32_t *hash);
int  m2sdr_get_clock_info(struct m2sdr_dev *dev, struct m2sdr_clock_info *info);

/* Register access */
int  m2sdr_reg_read(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
int  m2sdr_reg_write(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);

/* Low-level transport handle (for DMA/advanced use) */
int  m2sdr_get_fd(struct m2sdr_dev *dev);
void *m2sdr_get_handle(struct m2sdr_dev *dev);

/* DMA header control */
int  m2sdr_set_rx_header(struct m2sdr_dev *dev, bool enable, bool strip_header);
int  m2sdr_set_tx_header(struct m2sdr_dev *dev, bool enable);

/* GPIO helper (4-bit) */
int  m2sdr_gpio_config(struct m2sdr_dev *dev, bool enable, bool loopback, bool source_csr);
int  m2sdr_gpio_write(struct m2sdr_dev *dev, uint8_t value, uint8_t oe);
int  m2sdr_gpio_read(struct m2sdr_dev *dev, uint8_t *value);

/* Time */
int  m2sdr_get_time(struct m2sdr_dev *dev, uint64_t *time_ns);
int  m2sdr_set_time(struct m2sdr_dev *dev, uint64_t time_ns);
int  m2sdr_set_bitmode(struct m2sdr_dev *dev, bool enable_8bit);
int  m2sdr_set_dma_loopback(struct m2sdr_dev *dev, bool enable);
int  m2sdr_get_fpga_dna(struct m2sdr_dev *dev, uint64_t *dna);
int  m2sdr_get_fpga_sensors(struct m2sdr_dev *dev, struct m2sdr_fpga_sensors *sensors);

/* RF config */
void m2sdr_config_init(struct m2sdr_config *cfg);
int  m2sdr_apply_config(struct m2sdr_dev *dev, const struct m2sdr_config *cfg);

int  m2sdr_set_frequency(struct m2sdr_dev *dev, enum m2sdr_module m, uint64_t freq);
int  m2sdr_set_sample_rate(struct m2sdr_dev *dev, int64_t rate);
int  m2sdr_set_bandwidth(struct m2sdr_dev *dev, int64_t bw);
int  m2sdr_set_gain(struct m2sdr_dev *dev, enum m2sdr_module m, int64_t gain);

/* Streaming (BladeRF-like sync API) */
int m2sdr_sync_config(struct m2sdr_dev *dev,
                      enum m2sdr_module module,
                      enum m2sdr_format format,
                      unsigned num_buffers,
                      unsigned buffer_size,
                      unsigned num_transfers,
                      unsigned timeout_ms);

void m2sdr_sync_params_init(struct m2sdr_sync_params *params);
int m2sdr_sync_config_ex(struct m2sdr_dev *dev, const struct m2sdr_sync_params *params);

int m2sdr_sync_rx(struct m2sdr_dev *dev,
                  void *samples,
                  unsigned num_samples,
                  struct m2sdr_metadata *meta,
                  unsigned timeout_ms);

int m2sdr_sync_tx(struct m2sdr_dev *dev,
                  const void *samples,
                  unsigned num_samples,
                  struct m2sdr_metadata *meta,
                  unsigned timeout_ms);

/* Zero-copy buffer API */
int m2sdr_get_buffer(struct m2sdr_dev *dev,
                     enum m2sdr_module module,
                     void **buffer,
                     unsigned *num_samples,
                     unsigned timeout_ms);

int m2sdr_submit_buffer(struct m2sdr_dev *dev,
                        enum m2sdr_module module,
                        void *buffer,
                        unsigned num_samples,
                        struct m2sdr_metadata *meta);

int m2sdr_release_buffer(struct m2sdr_dev *dev,
                         enum m2sdr_module module,
                         void *buffer);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_H */
size_t m2sdr_format_size(enum m2sdr_format format);
void  *m2sdr_alloc_buffer(enum m2sdr_format format, unsigned num_samples);
void   m2sdr_free_buffer(void *buf);
