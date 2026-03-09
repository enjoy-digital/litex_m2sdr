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

/* Thread-safety notes:
 *
 * - A single m2sdr_dev instance is not safe for concurrent calls from multiple
 *   threads without external synchronization.
 * - Independent m2sdr_dev instances can be used concurrently.
 */

/* Public limits and fixed transport sizes used by the sync API. */
#define M2SDR_DEVICE_STR_MAX 256
#define M2SDR_SERIAL_MAX     32
#define M2SDR_IDENT_MAX      256
#define M2SDR_BUFFER_BYTES   8192u
#define M2SDR_HEADER_BYTES   16u

/* Error codes (0 on success, negative on failure).
 *
 * - PARSE: malformed text input (device identifiers, legacy mode strings).
 * - RANGE: numeric value outside supported device/library limits.
 * - STATE: API call ordering/state preconditions not met.
 */
enum m2sdr_error {
    M2SDR_ERR_OK         =  0,
    M2SDR_ERR_UNEXPECTED = -1,
    M2SDR_ERR_INVAL      = -2,
    M2SDR_ERR_IO         = -3,
    M2SDR_ERR_TIMEOUT    = -4,
    M2SDR_ERR_NO_MEM     = -5,
    M2SDR_ERR_UNSUPPORTED= -6,
    M2SDR_ERR_PARSE      = -7,
    M2SDR_ERR_RANGE      = -8,
    M2SDR_ERR_STATE      = -9,
};

/* libm2sdr starts its public compatibility story at v1.0.0. Keep the ABI
 * major in sync with the installed shared-library SONAME. */
#define M2SDR_API_VERSION 0x00010000
#define M2SDR_ABI_VERSION 0x00010000
#define M2SDR_VERSION_STRING "1.0.0"

struct m2sdr_version {
    uint32_t api;
    uint32_t abi;
    const char *version_str;
};

/* Return a short static string for a library error code. */
const char *m2sdr_strerror(int err);
/* Fill version information for the linked libm2sdr instance. */
void m2sdr_get_version(struct m2sdr_version *ver);

/* Data direction */
enum m2sdr_direction {
    M2SDR_RX = 0,
    M2SDR_TX = 1,
};

enum m2sdr_transport_kind {
    M2SDR_TRANSPORT_KIND_UNKNOWN = 0,
    M2SDR_TRANSPORT_KIND_LITEPCIE = 1,
    M2SDR_TRANSPORT_KIND_LITEETH = 2,
};

enum m2sdr_rfic_kind {
    M2SDR_RFIC_KIND_UNKNOWN = 0,
    M2SDR_RFIC_KIND_AD9361 = 1,
    M2SDR_RFIC_KIND_MOCK = 2,
};

enum m2sdr_rfic_feature_flag {
    M2SDR_RFIC_FEATURE_BIND_EXTERNAL = (1u << 0),
    M2SDR_RFIC_FEATURE_BIST          = (1u << 1),
    M2SDR_RFIC_FEATURE_OVERSAMPLE    = (1u << 2),
};

/* Backward-compatible alias for older code. */
typedef enum m2sdr_direction m2sdr_module_t;

/* Sample format (subset, extend as needed) */
enum m2sdr_format {
    /* 16-bit I/Q interleaved (SC16 Q11 style) */
    M2SDR_FORMAT_SC16_Q11 = 0,
    /* 8-bit I/Q interleaved (SC8 Q7 style) */
    M2SDR_FORMAT_SC8_Q7  = 1,
};

struct m2sdr_metadata {
    /* Timestamp in ns when M2SDR_META_FLAG_HAS_TIME is set. */
    uint64_t timestamp;
    /* Bitmask of M2SDR_META_FLAG_* values. */
    uint32_t flags;
};

#define M2SDR_META_FLAG_HAS_TIME (1u << 0)
#define M2SDR_IQ_BITS_MASK(bits) (1u << (bits))

struct m2sdr_sync_params {
    /* RX or TX stream to configure. */
    enum m2sdr_direction direction;
    /* Sample packing for the stream buffers. */
    enum m2sdr_format format;
    /* Backend ring buffer count. Use 0 for library defaults. */
    unsigned num_buffers;
    /* Samples per buffer, not bytes. */
    unsigned buffer_size;
    /* Reserved for API compatibility. Use 0 for defaults. */
    unsigned num_transfers;
    /* Blocking timeout for sync APIs in ms.
     * Passing 0 to per-call timeout arguments means "use this configured
     * timeout", not "non-blocking". */
    unsigned timeout_ms;
    /* Enable zero-copy PCIe DMA buffers when supported. */
    bool zero_copy;
    /* Prefix RX buffers with the FPGA DMA/VRT header. */
    bool rx_header_enable;
    /* Hide the RX header from user buffers when enabled. */
    bool rx_strip_header;
    /* Prefix TX buffers with the FPGA DMA/VRT header. */
    bool tx_header_enable;
};

typedef struct m2sdr_sync_params m2sdr_stream_config_t;

struct m2sdr_devinfo {
    /* Stable serial string when available. */
    char serial[M2SDR_SERIAL_MAX];
    /* SoC/bitstream identification string. */
    char identification[M2SDR_IDENT_MAX];
    /* Device path or transport address. */
    char path[M2SDR_DEVICE_STR_MAX];
    /* "litepcie" or "liteeth". */
    char transport[16];
};

struct m2sdr_capabilities {
    /* Gateware API version reported by the FPGA. */
    uint32_t api_version;
    /* Bitmask of M2SDR_FEATURE_* values. */
    uint32_t features;
    uint32_t board_info;
    uint32_t pcie_config;
    uint32_t eth_config;
    uint32_t sata_config;
};

struct m2sdr_clock_info {
    /* AD9361 reference clock frequency in Hz. */
    uint64_t refclk_hz;
    /* System clock frequency in Hz when available. */
    uint64_t sysclk_hz;
};

struct m2sdr_rfic_caps {
    enum m2sdr_rfic_kind kind;
    char name[32];
    uint32_t features;
    int64_t min_sample_rate;
    int64_t max_sample_rate;
    int64_t min_bandwidth;
    int64_t max_bandwidth;
    int64_t min_tx_gain;
    int64_t max_tx_gain;
    int64_t min_rx_gain;
    int64_t max_rx_gain;
    uint32_t supported_iq_bits_mask;
    uint8_t native_iq_bits;
    uint8_t min_iq_bits;
    uint8_t max_iq_bits;
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

enum m2sdr_channel_layout {
    M2SDR_CHANNEL_LAYOUT_2T2R = 0,
    M2SDR_CHANNEL_LAYOUT_1T1R = 1,
};

enum m2sdr_clock_source {
    M2SDR_CLOCK_SOURCE_INTERNAL = 0,
    M2SDR_CLOCK_SOURCE_EXTERNAL = 1,
};

/* RF configuration (matches existing utilities defaults) */
struct m2sdr_config {
    /* Common TX/RX sample rate in SPS. */
    int64_t sample_rate;
    /* Common TX/RX RF bandwidth in Hz. */
    int64_t bandwidth;
    /* AD9361 reference clock in Hz. */
    int64_t refclk_freq;
    /* TX LO in Hz. */
    int64_t tx_freq;
    /* RX LO in Hz. */
    int64_t rx_freq;
    /* TX gain in dB. Positive values mean more output power. */
    int64_t tx_gain;
    /* RX gain for channel 0 in dB. */
    int64_t rx_gain1;
    /* RX gain for channel 1 in dB. */
    int64_t rx_gain2;
    uint8_t loopback;
    bool    bist_tx_tone;
    bool    bist_rx_tone;
    bool    bist_prbs;
    int32_t bist_tone_freq;
    bool    enable_8bit_mode;
    bool    enable_oversample;
    /* Preferred typed RF topology controls. */
    enum m2sdr_channel_layout channel_layout;
    enum m2sdr_clock_source clock_source;
    /* Legacy string overrides kept for compatibility with old utilities. */
    const char *chan_mode; /* legacy string override: "2t2r" or "1t1r" */
    const char *sync_mode; /* legacy string override: "internal" or "external" */
};

struct m2sdr_dev;

/* Device management.
 *
 * Device identifiers are:
 * - "pcie:/dev/m2sdr0"
 * - "eth:192.168.1.50:1234"
 *
 * The parser also accepts the shorthand forms "/dev/m2sdr0" and
 * "192.168.1.50:1234". Passing NULL selects the backend default.
 */
int  m2sdr_open(struct m2sdr_dev **dev, const char *device_identifier);
void m2sdr_close(struct m2sdr_dev *dev);

/* Enumerate reachable devices for the active backend.
 *
 * PCIe currently probes /dev/m2sdr0..7. LiteEth currently probes only the
 * default target address rather than performing subnet discovery.
 */
int  m2sdr_get_device_list(struct m2sdr_devinfo *list, size_t max, size_t *count);

/* Read stable transport/path/serial/identifier metadata from an open device. */
int  m2sdr_get_device_info(struct m2sdr_dev *dev, struct m2sdr_devinfo *info);
int  m2sdr_get_capabilities(struct m2sdr_dev *dev, struct m2sdr_capabilities *caps);
int  m2sdr_get_identifier(struct m2sdr_dev *dev, char *buf, size_t len);
int  m2sdr_get_fpga_git_hash(struct m2sdr_dev *dev, uint32_t *hash);
int  m2sdr_get_clock_info(struct m2sdr_dev *dev, struct m2sdr_clock_info *info);

/* Register access.
 *
 * These are intentionally exposed for board-specific tooling, but most
 * applications should prefer higher-level helpers.
 */
int  m2sdr_reg_read(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
int  m2sdr_reg_write(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);

/* Low-level transport handle access for advanced integrations.
 *
 * `m2sdr_get_fd()` is meaningful only on LitePCIe builds.
 * `m2sdr_get_eb_handle()` is meaningful only on LiteEth builds.
 * `m2sdr_get_transport()` is the preferred way to branch on active backend.
 * `m2sdr_get_handle()` returns either the PCIe file descriptor cast to `void *`
 * or the Etherbone connection handle, depending on the active backend. On
 * LitePCIe builds this value is an integer fd reinterpreted as a pointer; it is
 * only for passing through opaque APIs and must not be dereferenced. Prefer
 * backend-specific accessors in new code.
 */
int  m2sdr_get_fd(struct m2sdr_dev *dev);
void *m2sdr_get_eb_handle(struct m2sdr_dev *dev);
int  m2sdr_get_transport(struct m2sdr_dev *dev, enum m2sdr_transport_kind *transport);
void *m2sdr_get_handle(struct m2sdr_dev *dev);
/* RFIC backend discovery and extension hooks.
 *
 * `m2sdr_get_rfic_name()` and `m2sdr_get_rfic_caps()` expose the currently
 * selected RFIC backend.
 * `m2sdr_set_property()` / `m2sdr_get_property()` use namespaced keys
 * (for example `ad9361.*`) for backend-specific controls.
 */
int  m2sdr_get_rfic_name(struct m2sdr_dev *dev, char *buf, size_t len);
int  m2sdr_get_rfic_caps(struct m2sdr_dev *dev, struct m2sdr_rfic_caps *caps);
int  m2sdr_set_property(struct m2sdr_dev *dev, const char *key, const char *value);
int  m2sdr_get_property(struct m2sdr_dev *dev, const char *key, char *value, size_t value_len);

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
int  m2sdr_set_iq_bits(struct m2sdr_dev *dev, unsigned bits);
int  m2sdr_get_iq_bits(struct m2sdr_dev *dev, unsigned *bits);
int  m2sdr_set_dma_loopback(struct m2sdr_dev *dev, bool enable);
int  m2sdr_get_fpga_dna(struct m2sdr_dev *dev, uint64_t *dna);
int  m2sdr_get_fpga_sensors(struct m2sdr_dev *dev, struct m2sdr_fpga_sensors *sensors);

/* RF config */
/* Initialize cfg with sane defaults matching the shipped utilities.
 *
 * This is the recommended starting point before overriding only the fields
 * your application cares about.
 */
void m2sdr_config_init(struct m2sdr_config *cfg);
/* Apply a full RF configuration to the currently-open device.
 *
 * This performs the same high-level bring-up sequence as `m2sdr_rf`: SI5351
 * clock selection, AD9361 SPI/RF init, channel layout, rates, gains, loopback,
 * and optional BIST modes.
 */
int  m2sdr_apply_config(struct m2sdr_dev *dev, const struct m2sdr_config *cfg);

/* Direction-based RF setters.
 *
 * These assume the AD9361 has already been initialized either through
 * `m2sdr_apply_config()` or an advanced integration path such as Soapy.
 */
int  m2sdr_set_frequency(struct m2sdr_dev *dev, enum m2sdr_direction direction, uint64_t freq);
int  m2sdr_get_frequency(struct m2sdr_dev *dev, enum m2sdr_direction direction, uint64_t *freq);
int  m2sdr_set_sample_rate(struct m2sdr_dev *dev, int64_t rate);
int  m2sdr_get_sample_rate(struct m2sdr_dev *dev, int64_t *rate);
int  m2sdr_set_bandwidth(struct m2sdr_dev *dev, int64_t bw);
int  m2sdr_get_bandwidth(struct m2sdr_dev *dev, int64_t *bw);
int  m2sdr_set_gain(struct m2sdr_dev *dev, enum m2sdr_direction direction, int64_t gain);
int  m2sdr_get_gain(struct m2sdr_dev *dev, enum m2sdr_direction direction, int64_t *gain);
/* Convenience RF setters for the common one-direction case. */
int  m2sdr_set_rx_frequency(struct m2sdr_dev *dev, uint64_t freq);
int  m2sdr_set_tx_frequency(struct m2sdr_dev *dev, uint64_t freq);
int  m2sdr_set_rx_gain(struct m2sdr_dev *dev, int64_t gain);
int  m2sdr_set_tx_gain(struct m2sdr_dev *dev, int64_t gain);
/* Advanced integration hook used by the SoapySDR driver. */
int  m2sdr_rf_bind(struct m2sdr_dev *dev, void *ad9361_phy);

/* Streaming (BladeRF-like sync API) */
/* Configure a stream directly.
 *
 * buffer_size is expressed in samples per buffer. Use
 * m2sdr_bytes_to_samples(M2SDR_FORMAT_..., M2SDR_BUFFER_BYTES)
 * for the default DMA payload size.
 */
int m2sdr_sync_config(struct m2sdr_dev *dev,
                      enum m2sdr_direction direction,
                      enum m2sdr_format format,
                      unsigned num_buffers,
                      unsigned buffer_size,
                      unsigned num_transfers,
                      unsigned timeout_ms);

/* Initialize the extended stream config with library defaults. */
void m2sdr_sync_params_init(struct m2sdr_sync_params *params);
/* Alias of m2sdr_sync_params_init() for the public stream-config name. */
void m2sdr_stream_config_init(m2sdr_stream_config_t *config);
/* Configure a stream with header/zero-copy options. */
int m2sdr_sync_config_ex(struct m2sdr_dev *dev, const struct m2sdr_sync_params *params);
/* Alias of m2sdr_sync_config_ex() for the public stream-config name. */
int m2sdr_stream_configure(struct m2sdr_dev *dev, const m2sdr_stream_config_t *config);

/* Blocking sync receive/transmit helpers. */
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

/* Zero-copy buffer API.
 *
 * These helpers expose backend-owned buffers directly. RX buffers are valid
 * until the next acquire on the same stream; TX buffers must be submitted with
 * m2sdr_submit_buffer(). Passing timeout_ms=0 uses the timeout configured by
 * m2sdr_sync_config()/m2sdr_sync_config_ex().
 *
 * DMA header layout (when enabled):
 * - Total size: 16 bytes.
 * - Bytes [0..7]: sync word 0x5aa55aa55aa55aa5.
 * - Bytes [8..15]: timestamp in nanoseconds.
 * - Endianness: values are encoded/decoded in native little-endian byte order
 *   as used by current host/FPGA flows.
 */
int m2sdr_get_buffer(struct m2sdr_dev *dev,
                     enum m2sdr_direction direction,
                     void **buffer,
                     unsigned *num_samples,
                     unsigned timeout_ms);

int m2sdr_submit_buffer(struct m2sdr_dev *dev,
                        enum m2sdr_direction direction,
                        void *buffer,
                        unsigned num_samples,
                        struct m2sdr_metadata *meta);

int m2sdr_release_buffer(struct m2sdr_dev *dev,
                         enum m2sdr_direction direction,
                         void *buffer);

/* Backend notes for zero-copy submit/release:
 *
 * - m2sdr_submit_buffer() performs an explicit enqueue on LiteEth, while on
 *   LitePCIe the DMA ring already owns the buffer and submit is a no-op.
 * - m2sdr_release_buffer() is currently a no-op kept for API symmetry.
 */

/* Small utility helpers for buffer sizing and allocation. */
size_t m2sdr_format_size(enum m2sdr_format format);
size_t m2sdr_samples_to_bytes(enum m2sdr_format format, unsigned num_samples);
unsigned m2sdr_bytes_to_samples(enum m2sdr_format format, size_t num_bytes);
void  *m2sdr_alloc_buffer(enum m2sdr_format format, unsigned num_samples);
void   m2sdr_free_buffer(void *buf);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_H */
