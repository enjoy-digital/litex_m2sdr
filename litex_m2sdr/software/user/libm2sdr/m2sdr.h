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

const char *m2sdr_strerror(int err);

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

struct m2sdr_devinfo {
    char serial[M2SDR_SERIAL_MAX];
    char identification[M2SDR_IDENT_MAX];
    char path[M2SDR_DEVICE_STR_MAX];
    char transport[16];
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

/* Register access */
int  m2sdr_readl(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
int  m2sdr_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);

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

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_H */
size_t m2sdr_format_size(enum m2sdr_format format);
void  *m2sdr_alloc_buffer(enum m2sdr_format format, unsigned num_samples);
void   m2sdr_free_buffer(void *buf);
