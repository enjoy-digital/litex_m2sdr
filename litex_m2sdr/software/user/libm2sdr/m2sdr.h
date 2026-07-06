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
#define M2SDR_BFP8_BLOCK_BYTES 1024u
#define M2SDR_BFP8_PAYLOAD_WORDS 127u
#define M2SDR_BFP8_SAMPLES_PER_CHANNEL 254u

/* Error codes (0 on success, negative on failure).
 *
 * - PARSE: malformed text input (device identifiers, legacy mode strings).
 * - RANGE: numeric value outside supported device/library limits.
 * - STATE: API call ordering/state preconditions not met.
 * - OVERFLOW/UNDERFLOW: streaming ring lost RX/TX continuity.
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
    M2SDR_ERR_OVERFLOW   = -10,
    M2SDR_ERR_UNDERFLOW  = -11,
};

/* libm2sdr starts its public compatibility story at v1.0.0. Keep the ABI
 * major in sync with the installed shared-library SONAME. */
#define M2SDR_API_VERSION 0x00010100
#define M2SDR_ABI_VERSION 0x00010000
#define M2SDR_VERSION_STRING "1.1.0"

struct m2sdr_version {
    uint32_t api;
    uint32_t abi;
    const char *version_str;
};

/* Return a short static string for a library error code. */
const char *m2sdr_strerror(int err);
/* Fill version information for the linked libm2sdr instance. */
void m2sdr_get_version(struct m2sdr_version *ver);
/* Enable or disable informational library logs. Errors are still reported. */
int m2sdr_set_log_enabled(bool enable);

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

enum m2sdr_sata_dma_direction {
    M2SDR_SATA_DMA_HOST_TO_DEVICE = 0,
    M2SDR_SATA_DMA_DEVICE_TO_HOST = 1,
};

#define M2SDR_SATA_PCIE_DMA_MAX_SECTORS 4096u

/* Backward-compatible alias for older code. */
typedef enum m2sdr_direction m2sdr_module_t;

/* Sample format (subset, extend as needed) */
enum m2sdr_format {
    /* 16-bit I/Q interleaved (SC16 Q11 style) */
    M2SDR_FORMAT_SC16_Q11 = 0,
    /* 8-bit I/Q interleaved (SC8 Q7 style) */
    M2SDR_FORMAT_SC8_Q7  = 1,
    /* Encoded BFP8 blocks: 1x64-bit header + 127x64-bit int8 mantissa payload words. */
    M2SDR_FORMAT_BFP8_Q11 = 2,
};

struct m2sdr_metadata {
    /* Timestamp in ns when M2SDR_META_FLAG_HAS_TIME is set. */
    uint64_t timestamp;
    /* Bitmask of M2SDR_META_FLAG_* values. */
    uint32_t flags;
};

#define M2SDR_META_FLAG_HAS_TIME (1u << 0)

/* Size of the optional FPGA DMA header (sync word + ns timestamp); see the
 * layout description above the zero-copy buffer API below. */
#define M2SDR_DMA_HEADER_SIZE 16

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

enum m2sdr_liteeth_rx_mode {
    M2SDR_LITEETH_RX_MODE_DISABLED = 0,
    M2SDR_LITEETH_RX_MODE_UDP      = 1,
    M2SDR_LITEETH_RX_MODE_VRT      = 2,
};

struct m2sdr_liteeth_rx_stream_config {
    enum m2sdr_liteeth_rx_mode mode;
    /* Host UDP listen port for raw UDP or VRT RX. */
    uint16_t udp_port;
    /* Host IPv4 address in host byte order. Use 0 for route auto-detection. */
    uint32_t local_ip;
};

struct m2sdr_liteeth_udp_stats {
    size_t buffer_size;
    size_t buffer_count;
    uint64_t rx_buffers;
    uint64_t rx_ring_full_events;
    uint64_t rx_flushes;
    uint64_t rx_flush_bytes;
    uint64_t rx_kernel_drops;
    uint64_t rx_source_drops;
    uint64_t rx_timeout_recoveries;
    uint64_t rx_recv_errors;
    uint64_t tx_buffers;
    uint64_t tx_bytes;
    uint64_t tx_send_errors;
    int so_rcvbuf_requested;
    int so_rcvbuf_actual;
    int so_sndbuf_requested;
    int so_sndbuf_actual;
};

struct m2sdr_stream_stats {
    enum m2sdr_transport_kind transport;
    enum m2sdr_direction direction;
    /* Backend descriptor payload size and ring depth. */
    size_t buffer_size;
    size_t buffer_count;
    /* Raw backend ring counters when available. */
    int64_t hw_count;
    int64_t sw_count;
    /* Current ring occupancy and observed high-water mark in descriptors. */
    uint64_t ring_level;
    uint64_t ring_high_water;
    /* RX late-consumer diagnostics. */
    uint64_t overflow_events;
    uint64_t overflow_buffers;
    /* TX late-producer diagnostics. */
    uint64_t underflow_events;
    uint64_t underflow_buffers;
    /* Transport-specific counters mirrored from LiteEth when available. */
    uint64_t rx_buffers;
    uint64_t tx_buffers;
    uint64_t rx_kernel_drops;
    uint64_t rx_source_drops;
    uint64_t rx_ring_full_events;
    uint64_t rx_timeout_recoveries;
    uint64_t rx_recv_errors;
    uint64_t tx_send_errors;
};

struct m2sdr_stream_info {
    /* Payload bytes visible to the caller, excluding stripped DMA headers. */
    size_t buffer_bytes;
    size_t buffer_count;
    /* Backend stride between direct buffers. May include transport headers. */
    size_t buffer_stride;
    /* Backend-owned direct buffer base when available. */
    void *buffer_base;
};

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

struct m2sdr_device_addr {
    enum m2sdr_transport_kind transport;
    /* Canonical identifier accepted by m2sdr_open(), eg pcie:/dev/m2sdr0. */
    char identifier[M2SDR_DEVICE_STR_MAX];
    char path[M2SDR_DEVICE_STR_MAX];
    char ip[64];
    uint16_t port;
};

struct m2sdr_discovery_config {
    bool enable_pcie;
    bool enable_liteeth;
    unsigned pcie_first;
    unsigned pcie_count;
    /* Comma- or semicolon-separated list of ip[:port] or eth:ip[:port] targets.
     * NULL selects the library default Ethernet target. */
    const char *liteeth_targets;
    /* Default port for Ethernet targets without an explicit port. Use 0 for
     * the library default. */
    uint16_t liteeth_port;
};

struct m2sdr_discovered_device {
    struct m2sdr_device_addr addr;
    struct m2sdr_devinfo info;
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

#define M2SDR_SATA_SERIAL_MAX   21
#define M2SDR_SATA_FIRMWARE_MAX 9
#define M2SDR_SATA_MODEL_MAX    41

struct m2sdr_sata_info {
    bool     phy_enabled;         /* SATA PHY enable bit is set.                  */
    uint32_t phy_status;          /* Raw SATA_PHY_STATUS register value.          */
    bool     phy_ready;           /* Decoded from phy_status.                     */
    bool     tx_ready;            /* Decoded from phy_status.                     */
    bool     rx_ready;            /* Decoded from phy_status.                     */
    bool     ctrl_ready;          /* Decoded from phy_status.                     */
    bool     drive_present;       /* identify_done and a non-zero sector count.   */
    bool     identify_done;       /* ATA IDENTIFY completed.                      */
    char     serial[M2SDR_SATA_SERIAL_MAX];     /* NUL-terminated, trimmed.       */
    char     firmware[M2SDR_SATA_FIRMWARE_MAX]; /* NUL-terminated, trimmed.       */
    char     model[M2SDR_SATA_MODEL_MAX];       /* NUL-terminated, trimmed.       */
    uint64_t sector_count;        /* Logical sector count (LBA48, else LBA28).    */
    uint32_t logical_sector_size; /* Bytes per logical sector (512 unless > 512). */
};

struct m2sdr_clock_info {
    /* AD9361 reference clock frequency in Hz. */
    uint64_t refclk_hz;
    /* System clock frequency in Hz when available. */
    uint64_t sysclk_hz;
};

struct m2sdr_ptp_port_identity {
    /* IEEE 1588 clockIdentity and portNumber. Zero values mean unknown. */
    uint64_t clock_id;
    uint16_t port_number;
};

/* Runtime policy for the Ethernet PTP board-time discipline loop. */
struct m2sdr_ptp_discipline_config {
    /* Allow PTP to own and steer the shared board TimeGenerator. */
    bool enable;
    /* Keep the last disciplined increment when PTP lock is lost. */
    bool holdover;
    /* Number of sys_clk cycles between discipline samples. */
    uint32_t update_cycles;
    /* First-lock / large-error threshold that requests an absolute time write. */
    uint32_t coarse_threshold_ns;
    /* Error threshold below which bounded phase trims are preferred. */
    uint32_t phase_threshold_ns;
    /* Error window considered locked after a valid PTP sample. */
    uint32_t lock_window_ns;
    /* Consecutive out-of-window samples required before declaring time unlock. */
    uint32_t unlock_misses;
    /* Consecutive runtime coarse errors required before allowing realignment. */
    uint32_t coarse_confirm;
    /* Right shift applied when converting phase error to phase-trim size. */
    uint8_t phase_step_shift;
    /* Maximum single bounded phase trim in ns. */
    uint32_t phase_step_max_ns;
    /* Signed TimeGenerator increment trim limit around the nominal increment. */
    uint32_t trim_limit;
    /* Proportional gain for TimeGenerator increment steering. */
    uint16_t p_gain;
};

/* Snapshot of Ethernet PTP board-time discipline state and counters. */
struct m2sdr_ptp_status {
    /* Configured enable bit; active means PTP currently owns board time. */
    bool enabled;
    bool active;
    /* ptp_locked tracks packet/reference validity; time_locked tracks local time error. */
    bool ptp_locked;
    bool time_locked;
    /* Holdover means the loop is retaining the last correction after PTP loss. */
    bool holdover;
    uint8_t state;
    /* Master IPv4 address in host byte order. */
    uint32_t master_ip;
    /* Current TimeGenerator fractional increment word. */
    uint32_t time_inc;
    /* Signed PTP time minus local board time for the last discipline sample. */
    int64_t last_error_ns;
    uint64_t last_ptp_time_ns;
    uint64_t last_local_time_ns;
    struct m2sdr_ptp_port_identity local_port;
    struct m2sdr_ptp_port_identity master_port;
    /* Learned sourcePortIdentity update count. */
    uint32_t identity_updates;
    /* Discipline action and loss counters, all wrapping 32-bit values. */
    uint32_t coarse_steps;
    uint32_t phase_steps;
    uint32_t rate_updates;
    uint32_t ptp_lock_losses;
    uint32_t time_lock_misses;
    uint32_t time_lock_miss_count;
    uint32_t time_lock_losses;
};

/* Runtime policy for the PTP-referenced FPGA 10MHz / RFIC clock loop. */
struct m2sdr_ptp_clock10_config {
    /* Allow the clk10 PI loop to override the MMCM dynamic phase-shift rate. */
    bool enable;
    /* Keep the last MMCM rate when the PTP reference is lost. */
    bool holdover;
    /* Invert MMCM phase-step sign when board wiring/MMCM direction requires it. */
    bool invert;
    /* Number of sys_clk cycles between MMCM rate accumulator updates. */
    uint32_t update_cycles;
    /* Q0.32 MMCM steps/update per sys_clk tick of phase error. */
    uint32_t p_gain;
    /* Q0.32 integral gain in MMCM steps/update per sys_clk tick of phase error. */
    uint32_t i_gain;
    /* Absolute Q0.32 MMCM steps/update limit for the PI command. */
    uint32_t rate_limit;
    /* clk10 marker lock window in sys_clk ticks. */
    uint32_t lock_window_ticks;
    /* Phase-detector search window in sys_clk ticks; normally half a second. */
    uint32_t half_period_ticks;
};

/* Snapshot of the PTP-to-10MHz discipline loop state and counters. */
struct m2sdr_ptp_clock10_status {
    /* Configured enable bit; active means the PTP clk10 loop owns MMCM steering. */
    bool enabled;
    bool active;
    /* reference_locked follows the board-time PTP reference. */
    bool reference_locked;
    /* clock_locked means the clk10 marker is inside lock_window_ticks. */
    bool clock_locked;
    bool holdover;
    /* aligned is set after automatic or manual 1Hz marker alignment. */
    bool aligned;
    /* Phase detector is waiting for the clk10 marker following a PTP reference. */
    bool waiting_after_ref;
    /* Most recent PI update saturated at rate_limit. */
    bool rate_limited;
    /* sys_clk phase-detector tick period in picoseconds. */
    uint32_t phase_tick_ps;
    /* Signed clk10 marker minus PTP reference phase error. */
    int32_t last_error_ticks;
    int64_t last_error_ns;
    /* Signed Q0.32 MMCM steps/update command. */
    int32_t last_rate;
    /* Phase-detector and PI-loop counters, all wrapping 32-bit values. */
    uint32_t sample_count;
    uint32_t reference_count;
    uint32_t clk10_count;
    uint32_t missing_count;
    uint32_t align_count;
    uint32_t lock_loss_count;
    uint32_t rate_update_count;
    uint32_t saturation_count;
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
#ifdef CSR_CAPABILITY_FEATURES_ETH_PTP_OFFSET
    M2SDR_FEATURE_ETH_PTP = 1u << CSR_CAPABILITY_FEATURES_ETH_PTP_OFFSET,
#else
    M2SDR_FEATURE_ETH_PTP = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_ETH_PTP_RFIC_CLOCK_OFFSET
    M2SDR_FEATURE_ETH_PTP_RFIC_CLOCK = 1u << CSR_CAPABILITY_FEATURES_ETH_PTP_RFIC_CLOCK_OFFSET,
#else
    M2SDR_FEATURE_ETH_PTP_RFIC_CLOCK = 0,
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
#ifdef CSR_CAPABILITY_FEATURES_ETH_PTP_SIZE
    M2SDR_FEATURE_ETH_PTP_MASK =
        ((1u << CSR_CAPABILITY_FEATURES_ETH_PTP_SIZE) - 1u) << CSR_CAPABILITY_FEATURES_ETH_PTP_OFFSET,
#else
    M2SDR_FEATURE_ETH_PTP_MASK = 0,
#endif
#ifdef CSR_CAPABILITY_FEATURES_ETH_PTP_RFIC_CLOCK_SIZE
    M2SDR_FEATURE_ETH_PTP_RFIC_CLOCK_MASK =
        ((1u << CSR_CAPABILITY_FEATURES_ETH_PTP_RFIC_CLOCK_SIZE) - 1u) <<
        CSR_CAPABILITY_FEATURES_ETH_PTP_RFIC_CLOCK_OFFSET,
#else
    M2SDR_FEATURE_ETH_PTP_RFIC_CLOCK_MASK = 0,
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
    M2SDR_CLOCK_SOURCE_INTERNAL      = 0,
    M2SDR_CLOCK_SOURCE_EXTERNAL      = 1,
    M2SDR_CLOCK_SOURCE_SI5351C_FPGA  = 2,
};

enum m2sdr_rx_gain_mode {
    M2SDR_RX_GAIN_MODE_MANUAL = 0,
    M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC = 1,
    M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC = 2,
    M2SDR_RX_GAIN_MODE_HYBRID_AGC = 3,
};

enum m2sdr_agc_detector {
    M2SDR_AGC_DETECTOR_RX1_LOW = 0,
    M2SDR_AGC_DETECTOR_RX1_HIGH = 1,
    M2SDR_AGC_DETECTOR_RX2_LOW = 2,
    M2SDR_AGC_DETECTOR_RX2_HIGH = 3,
};

struct m2sdr_agc_counter_config {
    bool enable;
    bool clear;
    uint16_t threshold;
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
    /* TX attenuation in dB. Higher values mean lower output power. */
    int64_t tx_att;
    /* RX gain for channel 0 in dB. */
    int64_t rx_gain1;
    /* RX gain for channel 1 in dB. */
    int64_t rx_gain2;
    /* Apply explicit RX gain programming and force manual gain mode. Enabled
     * by m2sdr_config_init(); clear this and set program_rx_gain_modes when
     * using AD9361 AGC modes instead. */
    bool    program_rx_gains;
    /* Apply explicit AD9361 RX gain-control modes when not programming manual gains. */
    bool    program_rx_gain_modes;
    enum m2sdr_rx_gain_mode rx_gain_mode1;
    enum m2sdr_rx_gain_mode rx_gain_mode2;
    /* Drive the FPGA-connected AD9361 EN_AGC pin for pin-controlled AGC flows. */
    bool    program_agc_pin;
    bool    agc_pin_enable;
    uint8_t loopback;
    bool    bist_tx_tone;
    bool    bist_rx_tone;
    bool    bist_prbs;
    /* Scan/program FPGA <-> AD9361 interface delays using the PRBS path. */
    bool    calibrate_interface_delay;
    int32_t bist_tone_freq;
    bool    enable_8bit_mode;
    enum m2sdr_format sample_format;
    /* Preferred typed RF topology controls. */
    enum m2sdr_channel_layout channel_layout;
    enum m2sdr_clock_source clock_source;
    /* Legacy string overrides kept for compatibility with old utilities. */
    const char *chan_mode; /* legacy string override: "2t2r" or "1t1r" */
    const char *sync_mode; /* legacy string override: "internal", "external", or "fpga" */
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
int  m2sdr_resolve_device_identifier(const char *device_identifier,
                                      struct m2sdr_device_addr *addr);
int  m2sdr_open(struct m2sdr_dev **dev, const char *device_identifier);
void m2sdr_close(struct m2sdr_dev *dev);

/* Initialize discovery policy with the library defaults:
 * PCIe /dev/m2sdr0..7 followed by Ethernet target 192.168.1.50:1234. */
void m2sdr_discovery_config_init(struct m2sdr_discovery_config *config);

/* Expand a discovery policy into canonical candidate identifiers. This does
 * not open devices; use m2sdr_get_device_list_ex() to probe reachability. */
int  m2sdr_get_discovery_targets(const struct m2sdr_discovery_config *config,
                                 struct m2sdr_device_addr *targets,
                                 size_t max,
                                 size_t *count);

/* Enumerate reachable devices for a configured discovery policy.
 *
 * Discovery is bounded to the configured PCIe indices and Ethernet targets; it
 * never performs a subnet scan.
 */
int  m2sdr_get_device_list_ex(const struct m2sdr_discovery_config *config,
                              struct m2sdr_discovered_device *list,
                              size_t max,
                              size_t *count);
int  m2sdr_get_device_list(struct m2sdr_devinfo *list, size_t max, size_t *count);

/* Read stable transport/path/serial/identifier metadata from an open device. */
int  m2sdr_get_device_info(struct m2sdr_dev *dev, struct m2sdr_devinfo *info);
int  m2sdr_get_capabilities(struct m2sdr_dev *dev, struct m2sdr_capabilities *caps);
int  m2sdr_get_identifier(struct m2sdr_dev *dev, char *buf, size_t len);
int  m2sdr_get_fpga_git_hash(struct m2sdr_dev *dev, uint32_t *hash);
int  m2sdr_get_clock_info(struct m2sdr_dev *dev, struct m2sdr_clock_info *info);
int  m2sdr_get_sata_info(struct m2sdr_dev *dev, struct m2sdr_sata_info *info, unsigned timeout_ms);

/* Register access.
 *
 * These are intentionally exposed for board-specific tooling, but most
 * applications should prefer higher-level helpers.
 */
int  m2sdr_reg_read(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val);
int  m2sdr_reg_write(struct m2sdr_dev *dev, uint32_t addr, uint32_t val);
int  m2sdr_reg_read_bulk(struct m2sdr_dev *dev, uint32_t addr, uint32_t *vals, size_t count);
int  m2sdr_reg_write_bulk(struct m2sdr_dev *dev, uint32_t addr, const uint32_t *vals, size_t count);

/* Copy SATA sectors to/from host memory using the LitePCIe userspace DMA path.
 *
 * 'buf' must hold at least nsectors * 512 bytes. 'timeout_ms' bounds each DMA
 * chunk (< 0 waits forever). On return '*transferred' (if non-NULL) holds the
 * number of sectors actually copied, even on error, so a partial transfer can
 * be resumed. Returns M2SDR_ERR_UNSUPPORTED on non-PCIe transports.
 */
int  m2sdr_sata_pcie_dma_copy(struct m2sdr_dev *dev,
                              enum m2sdr_sata_dma_direction direction,
                              uint64_t sector,
                              uint32_t nsectors,
                              void *buf,
                              int timeout_ms,
                              uint32_t *transferred);

/* Low-level transport handle access for advanced integrations.
 *
 * `m2sdr_get_fd()` returns a valid descriptor only for LitePCIe devices.
 * `m2sdr_get_eb_handle()` returns a valid handle only for LiteEth devices.
 * `m2sdr_get_transport()` is the preferred way to branch on active backend.
 * `m2sdr_get_handle()` returns either the PCIe file descriptor cast to `void *`
 * or the Etherbone connection handle, depending on the active backend. On
 * LitePCIe devices this value is an integer fd reinterpreted as a pointer; it
 * is only for passing through opaque APIs and must not be dereferenced. Prefer
 * backend-specific accessors in new code.
 */
int  m2sdr_get_fd(struct m2sdr_dev *dev);
void *m2sdr_get_eb_handle(struct m2sdr_dev *dev);
int  m2sdr_get_transport(struct m2sdr_dev *dev, enum m2sdr_transport_kind *transport);
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
int  m2sdr_get_ptp_status(struct m2sdr_dev *dev, struct m2sdr_ptp_status *status);
int  m2sdr_get_ptp_discipline_config(struct m2sdr_dev *dev, struct m2sdr_ptp_discipline_config *cfg);
int  m2sdr_set_ptp_discipline_config(struct m2sdr_dev *dev, const struct m2sdr_ptp_discipline_config *cfg);
int  m2sdr_clear_ptp_counters(struct m2sdr_dev *dev);
int  m2sdr_get_ptp_clock10_status(struct m2sdr_dev *dev, struct m2sdr_ptp_clock10_status *status);
int  m2sdr_get_ptp_clock10_config(struct m2sdr_dev *dev, struct m2sdr_ptp_clock10_config *cfg);
int  m2sdr_set_ptp_clock10_config(struct m2sdr_dev *dev, const struct m2sdr_ptp_clock10_config *cfg);
int  m2sdr_clear_ptp_clock10_counters(struct m2sdr_dev *dev);
/* Queue a clk10 marker realignment; hardware applies it on the next PTP reference edge. */
int  m2sdr_align_ptp_clock10(struct m2sdr_dev *dev);
int  m2sdr_set_agc_pin(struct m2sdr_dev *dev, bool enable);
int  m2sdr_get_agc_pin(struct m2sdr_dev *dev, bool *enabled);
int  m2sdr_configure_agc_counter(struct m2sdr_dev *dev,
                                 enum m2sdr_agc_detector detector,
                                 const struct m2sdr_agc_counter_config *config);
int  m2sdr_clear_agc_counter(struct m2sdr_dev *dev, enum m2sdr_agc_detector detector);
int  m2sdr_get_agc_count(struct m2sdr_dev *dev, enum m2sdr_agc_detector detector,
                         uint32_t *count);
int  m2sdr_set_sample_format(struct m2sdr_dev *dev, enum m2sdr_format format);
int  m2sdr_set_bitmode(struct m2sdr_dev *dev, bool enable_8bit);
int  m2sdr_set_dma_loopback(struct m2sdr_dev *dev, bool enable);
int  m2sdr_set_txrx_loopback(struct m2sdr_dev *dev, bool enable);
int  m2sdr_set_rfic_data_loopback(struct m2sdr_dev *dev, bool enable);
int  m2sdr_get_rfic_data_loopback(struct m2sdr_dev *dev, bool *enabled);
int  m2sdr_reset_datapath(struct m2sdr_dev *dev);
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
/* Apply cfg only when no RF config has been applied yet, or when cfg matches
 * the last successfully applied config. This avoids accidental AD9361 re-init
 * while letting startup code call the helper idempotently. Returns
 * M2SDR_ERR_STATE when the RFIC is already initialized with a different config. */
int  m2sdr_apply_config_if_needed(struct m2sdr_dev *dev,
                                  const struct m2sdr_config *cfg);

/* Direction-based RF setters.
 *
 * These assume the AD9361 has already been initialized either through
 * `m2sdr_apply_config()` or an advanced integration path such as Soapy.
 */
int  m2sdr_set_frequency(struct m2sdr_dev *dev, enum m2sdr_direction direction, uint64_t freq);
int  m2sdr_set_sample_rate(struct m2sdr_dev *dev, int64_t rate);
int  m2sdr_set_bandwidth(struct m2sdr_dev *dev, int64_t bw);
int  m2sdr_set_gain(struct m2sdr_dev *dev, enum m2sdr_direction direction, int64_t gain);
/* Switch the 1T1R/2T2R channel layout. Re-initializes the AD9361 from the
 * shared init parameters (the device must have completed RF bring-up once),
 * going through the same configuration code as the init-time path. Callers
 * caching the phy handle must refresh it via m2sdr_rf_phy() afterwards, and
 * re-apply their RF settings (rates, frequencies, gains). */
int  m2sdr_set_channel_mode(struct m2sdr_dev *dev, unsigned channel_count,
                            unsigned rx_channel, unsigned tx_channel);
const char *m2sdr_rx_gain_mode_name(enum m2sdr_rx_gain_mode mode);
int  m2sdr_parse_rx_gain_mode(const char *text, enum m2sdr_rx_gain_mode *mode);
int  m2sdr_set_rx_gain_mode_all(struct m2sdr_dev *dev, enum m2sdr_rx_gain_mode mode);
/* Stream sample rate as observed by the host (inverse of the conversions
 * applied by m2sdr_set_sample_rate()). */
int  m2sdr_get_sample_rate(struct m2sdr_dev *dev, int64_t *rate);
/* Currently bound AD9361 phy handle (NULL when uninitialized). */
void *m2sdr_rf_phy(struct m2sdr_dev *dev);
int  m2sdr_set_rx_gain_mode(struct m2sdr_dev *dev, unsigned channel,
                            enum m2sdr_rx_gain_mode mode);
int  m2sdr_get_rx_gain_mode(struct m2sdr_dev *dev, unsigned channel,
                            enum m2sdr_rx_gain_mode *mode);
/* Convenience RF setters for the common one-direction case. */
int  m2sdr_set_rx_frequency(struct m2sdr_dev *dev, uint64_t freq);
int  m2sdr_set_tx_frequency(struct m2sdr_dev *dev, uint64_t freq);
int  m2sdr_set_rx_gain(struct m2sdr_dev *dev, int64_t gain);
int  m2sdr_set_rx_gain_chan(struct m2sdr_dev *dev, unsigned channel, int64_t gain);
int  m2sdr_set_tx_att(struct m2sdr_dev *dev, int64_t attenuation_db);
int  m2sdr_set_rfic_loopback(struct m2sdr_dev *dev, uint8_t mode);
int  m2sdr_set_fpga_prbs_tx(struct m2sdr_dev *dev, bool enable);
int  m2sdr_get_fpga_prbs_rx_synced(struct m2sdr_dev *dev, bool *synced);
/* Advanced integration hook used by the SoapySDR driver. */
int  m2sdr_rf_bind(struct m2sdr_dev *dev, void *ad9361_phy);
/* Store the AD9361_InitParam an external integration initialized the RFIC
 * with, so m2sdr_set_channel_mode() re-initializes with the same reference
 * clock, SPI id and GPIO setup. init_param must point to an AD9361_InitParam
 * and size must be sizeof(AD9361_InitParam) (guards ABI mismatches). */
int  m2sdr_rf_store_init_param(struct m2sdr_dev *dev,
                               const void *init_param, size_t size);
/* Optional override of the AD9361 platform hooks libm2sdr provides as
 * defaults for the standalone tools. An external integration (e.g. the
 * SoapySDR module) registers its own set once before RF init; NULL members
 * keep the default behavior. Registration replaces the strong-symbol
 * override, which needs -Wl,--allow-multiple-definition on PE-COFF where
 * weak symbols do not resolve. */
struct spi_device;
struct m2sdr_rf_platform_hooks {
    int  (*spi_write_then_read)(struct spi_device *spi,
                                const unsigned char *txbuf, unsigned n_tx,
                                unsigned char *rxbuf, unsigned n_rx);
    void (*udelay)(unsigned long usecs);
    void (*mdelay)(unsigned long msecs);
    unsigned long (*msleep_interruptible)(unsigned int msecs);
    bool (*gpio_is_valid)(int number);
    void (*gpio_set_value)(unsigned gpio, int value);
};
void m2sdr_rf_set_platform_hooks(const struct m2sdr_rf_platform_hooks *hooks);

/* Streaming (BladeRF-like sync API) */
/* Configure a stream directly.
 *
 * buffer_size is the fixed backend descriptor payload size expressed in
 * samples per buffer, not the size of each m2sdr_sync_rx()/m2sdr_sync_tx()
 * request. For BFP8, one sample is one encoded M2SDR_BFP8_BLOCK_BYTES block.
 * Use
 * m2sdr_bytes_to_samples(M2SDR_FORMAT_..., M2SDR_BUFFER_BYTES)
 * for the default DMA payload size. Larger per-call requests are passed to
 * m2sdr_sync_rx()/m2sdr_sync_tx(), which drain/fill multiple descriptors.
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
int m2sdr_stream_get_info(struct m2sdr_dev *dev,
                          enum m2sdr_direction direction,
                          struct m2sdr_stream_info *info);
int m2sdr_get_stream_stats(struct m2sdr_dev *dev,
                           enum m2sdr_direction direction,
                           struct m2sdr_stream_stats *stats);
int m2sdr_clear_stream_stats(struct m2sdr_dev *dev,
                             enum m2sdr_direction direction);
int m2sdr_stream_deactivate(struct m2sdr_dev *dev, enum m2sdr_direction direction);
/* Re-enable a previously configured (and possibly deactivated) stream. On
 * LitePCIe this resyncs the userspace ring counters with the kernel, whose
 * counters restart across a stop/start cycle. */
int m2sdr_stream_activate(struct m2sdr_dev *dev, enum m2sdr_direction direction);
int m2sdr_stream_release(struct m2sdr_dev *dev, enum m2sdr_direction direction);

/* LiteEth stream-control helpers.
 *
 * These functions are additive helpers for integrations that need explicit
 * setup/activate/deactivate control instead of the blocking sync API.
 */
void m2sdr_liteeth_rx_stream_config_init(struct m2sdr_liteeth_rx_stream_config *config);
int m2sdr_liteeth_get_local_ip(struct m2sdr_dev *dev, uint32_t *local_ip);
int m2sdr_liteeth_rx_stream_prepare(struct m2sdr_dev *dev,
                                    const struct m2sdr_liteeth_rx_stream_config *config);
int m2sdr_liteeth_rx_stream_activate(struct m2sdr_dev *dev,
                                     const struct m2sdr_liteeth_rx_stream_config *config);
int m2sdr_liteeth_rx_stream_deactivate(struct m2sdr_dev *dev);
int m2sdr_liteeth_tx_stream_activate(struct m2sdr_dev *dev);
int m2sdr_liteeth_tx_stream_deactivate(struct m2sdr_dev *dev);
int m2sdr_liteeth_set_rx_timeout_recovery(struct m2sdr_dev *dev, bool enable);
int m2sdr_liteeth_get_udp_stats(struct m2sdr_dev *dev,
                                struct m2sdr_liteeth_udp_stats *stats);

/* Blocking sync receive/transmit helpers.
 *
 * timeout_ms = 0 uses the stream timeout configured through
 * m2sdr_sync_config()/m2sdr_sync_config_ex().
 *
 * num_samples is the requested transfer size and may be larger than the
 * descriptor size configured with m2sdr_sync_config().
 *
 * When RX headers are enabled, meta carries the hardware timestamp of the
 * FIRST sample of the returned block (a request spanning several DMA buffers
 * keeps the first buffer's timestamp). */
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
 * These helpers expose backend-owned buffers directly. RX buffers must be
 * returned with m2sdr_release_buffer(); TX buffers must be submitted with
 * m2sdr_submit_buffer(). Passing timeout_ms=0 uses the timeout configured by
 * m2sdr_sync_config()/m2sdr_sync_config_ex(). m2sdr_try_get_buffer() is
 * non-blocking and returns M2SDR_ERR_TIMEOUT when no buffer is immediately
 * available.
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

int m2sdr_try_get_buffer(struct m2sdr_dev *dev,
                         enum m2sdr_direction direction,
                         void **buffer,
                         unsigned *num_samples);

int m2sdr_submit_buffer(struct m2sdr_dev *dev,
                        enum m2sdr_direction direction,
                        void *buffer,
                        unsigned num_samples,
                        struct m2sdr_metadata *meta);

int m2sdr_release_buffer(struct m2sdr_dev *dev,
                         enum m2sdr_direction direction,
                         void *buffer);

/* Retrieve the metadata carried by the DMA header of an RX buffer obtained
 * from m2sdr_get_buffer()/m2sdr_try_get_buffer(). meta->flags carries
 * M2SDR_META_FLAG_HAS_TIME (with meta->timestamp in ns) only when RX headers
 * are enabled and the buffer holds a valid sync word; otherwise meta is
 * cleared and the call still succeeds, so it can be issued unconditionally. */
int m2sdr_get_buffer_metadata(struct m2sdr_dev *dev,
                              enum m2sdr_direction direction,
                              const void *buffer,
                              struct m2sdr_metadata *meta);

/* Backend notes for zero-copy submit/release:
 *
 * - m2sdr_submit_buffer() performs an explicit enqueue on LiteEth and advances
 *   the kernel-visible TX ring on LitePCIe zero-copy streams.
 * - m2sdr_release_buffer() advances the kernel-visible RX ring on LitePCIe
 *   zero-copy streams; other current paths advance on read.
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
