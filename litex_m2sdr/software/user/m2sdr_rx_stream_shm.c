/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Stream to Shared Memory Utility.
 *
 * This utility configures the AD9361 RF frontend and streams RX data
 * directly to a shared memory ring buffer. Designed to run as a separate
 * process from Julia signal processing to avoid GC pause interference.
 *
 * Data Format:
 *   - Each DMA buffer is copied directly to a shared memory slot
 *   - Samples are Complex{Int16} (4 bytes per sample per channel)
 *   - Multi-channel data is interleaved: I0,Q0,I1,Q1,I0,Q0,I1,Q1,...
 *   - 12-bit ADC samples are sign-extended to 16-bit
 *
 * Shared Memory Header Layout (64 bytes):
 *   Bytes 0-7:   write_index (uint64_t) - next slot to write
 *   Bytes 8-15:  read_index (uint64_t) - next slot to read
 *   Bytes 16-23: chunks_written (uint64_t) - total chunks written
 *   Bytes 24-31: chunks_read (uint64_t) - total chunks read
 *   Bytes 32-35: chunk_size (uint32_t) - samples per chunk (per channel)
 *   Bytes 36-39: sample_size (uint32_t) - bytes per sample (4 for Complex{Int16})
 *   Bytes 40-43: num_slots (uint32_t) - number of slots in buffer
 *   Bytes 44-45: num_channels (uint16_t) - number of channels (1 or 2)
 *   Bytes 46-47: flags (uint16_t) - bit 0 = writer_done
 *   Bytes 48-55: overflow_count (uint64_t) - number of DMA overflows
 *   Bytes 56-63: lost_samples (uint64_t) - total samples lost to overflow
 *
 * Per-Chunk Header Layout (8 bytes, prepended to each slot):
 *   Bytes 0-7:   samples_lost_before (uint64_t) - samples lost since previous chunk
 *
 * Slot Layout:
 *   [8-byte chunk header][sample data...]
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "m2sdr_config.h"

#include "liblitepcie.h"
#include "libm2sdr.h"

/* Shared memory header offsets (0-indexed for C) */
#define SHM_HEADER_SIZE         64
#define OFFSET_WRITE_INDEX      0
#define OFFSET_READ_INDEX       8
#define OFFSET_CHUNKS_WRITTEN   16
#define OFFSET_CHUNKS_READ      24
#define OFFSET_CHUNK_SIZE       32
#define OFFSET_SAMPLE_SIZE      36
#define OFFSET_NUM_SLOTS        40
#define OFFSET_NUM_CHANNELS     44
#define OFFSET_FLAGS            46
#define OFFSET_OVERFLOW_COUNT   48
#define OFFSET_LOST_SAMPLES     56

/* Flag bits */
#define FLAG_WRITER_DONE        1

/* Per-chunk header size (prepended to each slot's sample data)
 * Bytes 0-7: samples_lost_before (uint64_t) - samples lost since previous chunk
 */
#define CHUNK_HEADER_SIZE       8

/* Sample size for Complex{Int16} (real + imag, each int16) */
#define SAMPLE_SIZE_COMPLEX_INT16 4

/* Sign-extend 12-bit samples to 16-bit in-place
 * AD9361 outputs 12-bit signed samples in 16-bit words.
 * Bit 11 is the sign bit, so we need to extend it to bit 15.
 */
static inline void sign_extend_12bit_inplace(int16_t *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = (int16_t)(data[i] << 4) >> 4;
    }
}

/* Variables */
/*-----------*/

static char m2sdr_device[1024];
static int m2sdr_device_num = 0;

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

/* forward (provided in tree elsewhere; used here) */
extern int64_t get_time_ms(void);

/* AD9361 */
/*--------*/

#define AD9361_GPIO_RESET_PIN 0

struct ad9361_rf_phy *ad9361_phy;

static void * m2sdr_open(void) {
    int fd = open(m2sdr_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open device %s\n", m2sdr_device);
        exit(1);
    }
    return (void *)(intptr_t)fd;
}

static void m2sdr_close(void *conn) {
    close((int)(intptr_t)conn);
}

int spi_write_then_read(struct spi_device *spi,
                        const unsigned char *txbuf, unsigned n_tx,
                        unsigned char *rxbuf, unsigned n_rx)
{
    void *conn = m2sdr_open();

    if (n_tx == 2 && n_rx == 1) {
        rxbuf[0] = m2sdr_ad9361_spi_read(conn, txbuf[0] << 8 | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        m2sdr_ad9361_spi_write(conn, txbuf[0] << 8 | txbuf[1], txbuf[2]);
    } else {
        fprintf(stderr, "Unsupported SPI transfer n_tx=%d n_rx=%d\n", n_tx, n_rx);
        m2sdr_close(conn);
        exit(1);
    }

    m2sdr_close(conn);
    return 0;
}

void udelay(unsigned long usecs) { usleep(usecs); }
void mdelay(unsigned long msecs) { usleep(msecs * 1000); }
unsigned long msleep_interruptible(unsigned int msecs) { usleep(msecs * 1000); return 0; }

bool gpio_is_valid(int number) {
    return (number == AD9361_GPIO_RESET_PIN);
}

void gpio_set_value(unsigned gpio, int value) {
    (void)gpio; (void)value;
}

/* Shared Memory Ring Buffer */
/*---------------------------*/

typedef struct {
    uint8_t *data;
    size_t   total_size;
    uint32_t num_slots;
    uint32_t chunk_size;      /* samples per chunk (per channel) */
    uint32_t chunk_bytes;     /* bytes per chunk (all channels) */
    uint16_t num_channels;
} shm_ring_buffer_t;

/* RX: C is the producer, Julia is the consumer.
 * C writes samples then updates write_index with release semantics.
 * Julia reads write_index with acquire semantics to see the samples.
 * Julia updates read_index with release semantics after consuming.
 * C reads read_index with acquire semantics before overwriting slots. */

static inline uint64_t shm_get_write_index(shm_ring_buffer_t *shm) {
    return *(volatile uint64_t *)(shm->data + OFFSET_WRITE_INDEX);
}

static inline void shm_set_write_index(shm_ring_buffer_t *shm, uint64_t val) {
    /* Release: ensures sample data is visible before index update */
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_WRITE_INDEX), val, __ATOMIC_RELEASE);
}

static inline uint64_t shm_get_read_index(shm_ring_buffer_t *shm) {
    /* Acquire: ensures we see Julia consumer's release-store of read_index */
    return __atomic_load_n((volatile uint64_t *)(shm->data + OFFSET_READ_INDEX), __ATOMIC_ACQUIRE);
}

static inline void shm_set_chunks_written(shm_ring_buffer_t *shm, uint64_t val) {
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_CHUNKS_WRITTEN), val, __ATOMIC_RELAXED);
}

static inline void shm_set_overflow_count(shm_ring_buffer_t *shm, uint64_t val) {
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_OVERFLOW_COUNT), val, __ATOMIC_RELAXED);
}

static inline void shm_set_lost_samples(shm_ring_buffer_t *shm, uint64_t val) {
    *(volatile uint64_t *)(shm->data + OFFSET_LOST_SAMPLES) = val;
}

static inline void shm_set_flags(shm_ring_buffer_t *shm, uint16_t val) {
    *(volatile uint16_t *)(shm->data + OFFSET_FLAGS) = val;
}

static inline uint16_t shm_get_flags(shm_ring_buffer_t *shm) {
    return *(volatile uint16_t *)(shm->data + OFFSET_FLAGS);
}

static inline void shm_mark_writer_done(shm_ring_buffer_t *shm) {
    shm_set_flags(shm, shm_get_flags(shm) | FLAG_WRITER_DONE);
}

static inline bool shm_can_write(shm_ring_buffer_t *shm) {
    uint64_t write_idx = shm_get_write_index(shm);
    uint64_t read_idx = shm_get_read_index(shm);
    return (write_idx - read_idx) < shm->num_slots;
}

/* Get pointer to start of slot (includes chunk header) */
static inline uint8_t *shm_slot_ptr(shm_ring_buffer_t *shm, uint64_t slot) {
    size_t slot_size = CHUNK_HEADER_SIZE + shm->chunk_bytes;
    size_t offset = SHM_HEADER_SIZE + (slot % shm->num_slots) * slot_size;
    return shm->data + offset;
}

/* Set per-chunk header: samples lost before this chunk */
static inline void shm_set_chunk_lost_samples(uint8_t *slot_ptr, uint64_t samples_lost) {
    *(uint64_t *)(slot_ptr) = samples_lost;
}

/* Create shared memory ring buffer
 * chunk_size = DMA_BUFFER_SIZE (no rechunking)
 * Each slot holds: [16-byte chunk header] + [sample data]
 */
static shm_ring_buffer_t *shm_create(const char *path, uint32_t chunk_bytes,
                                      uint16_t num_channels, double buffer_seconds,
                                      uint32_t sample_rate)
{
    shm_ring_buffer_t *shm = calloc(1, sizeof(shm_ring_buffer_t));
    if (!shm) {
        perror("calloc");
        return NULL;
    }

    shm->num_channels = num_channels;
    shm->chunk_bytes = chunk_bytes;
    /* samples per chunk per channel */
    shm->chunk_size = chunk_bytes / (SAMPLE_SIZE_COMPLEX_INT16 * num_channels);

    /* Calculate number of slots for requested buffer time */
    uint64_t bytes_per_second = (uint64_t)sample_rate * SAMPLE_SIZE_COMPLEX_INT16 * num_channels;
    uint64_t total_buffer_bytes = (uint64_t)(bytes_per_second * buffer_seconds);
    shm->num_slots = (total_buffer_bytes + chunk_bytes - 1) / chunk_bytes;
    if (shm->num_slots < 16) shm->num_slots = 16;  /* minimum slots */

    /* Each slot = chunk header + sample data */
    size_t slot_size = CHUNK_HEADER_SIZE + shm->chunk_bytes;
    shm->total_size = SHM_HEADER_SIZE + (size_t)shm->num_slots * slot_size;

    /* Create shared memory file */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror(path);
        free(shm);
        return NULL;
    }

    if (ftruncate(fd, shm->total_size) < 0) {
        perror("ftruncate");
        close(fd);
        free(shm);
        return NULL;
    }

    shm->data = mmap(NULL, shm->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm->data == MAP_FAILED) {
        perror("mmap");
        free(shm);
        return NULL;
    }

    /* Initialize header */
    memset(shm->data, 0, SHM_HEADER_SIZE);
    *(uint32_t *)(shm->data + OFFSET_CHUNK_SIZE) = shm->chunk_size;
    *(uint32_t *)(shm->data + OFFSET_SAMPLE_SIZE) = SAMPLE_SIZE_COMPLEX_INT16;
    *(uint32_t *)(shm->data + OFFSET_NUM_SLOTS) = shm->num_slots;
    *(uint16_t *)(shm->data + OFFSET_NUM_CHANNELS) = num_channels;

    printf("Created shared memory ring buffer:\n");
    printf("  Path: %s\n", path);
    printf("  Chunk size: %u bytes (%u samples/channel)\n", chunk_bytes, shm->chunk_size);
    printf("  Num channels: %u\n", num_channels);
    printf("  Num slots: %u\n", shm->num_slots);
    printf("  Buffer size: %.1f MB\n", shm->total_size / 1024.0 / 1024.0);
    printf("  Buffer time: %.2f seconds\n",
           (double)shm->num_slots * shm->chunk_size / sample_rate);

    return shm;
}

static void shm_destroy(shm_ring_buffer_t *shm) {
    if (shm) {
        if (shm->data && shm->data != MAP_FAILED) {
            shm_mark_writer_done(shm);
            munmap(shm->data, shm->total_size);
        }
        free(shm);
    }
}

/* AGC Mode Parsing */
/*------------------*/

static const char *agc_mode_names[] = {
    "manual",
    "fast_attack",
    "slow_attack",
    "hybrid"
};

static uint8_t parse_agc_mode(const char *mode_str) {
    if (strcasecmp(mode_str, "manual") == 0 || strcasecmp(mode_str, "mgc") == 0)
        return RF_GAIN_MGC;
    if (strcasecmp(mode_str, "fast_attack") == 0 || strcasecmp(mode_str, "fast") == 0)
        return RF_GAIN_FASTATTACK_AGC;
    if (strcasecmp(mode_str, "slow_attack") == 0 || strcasecmp(mode_str, "slow") == 0)
        return RF_GAIN_SLOWATTACK_AGC;
    if (strcasecmp(mode_str, "hybrid") == 0)
        return RF_GAIN_HYBRID_AGC;
    fprintf(stderr, "Error: unknown AGC mode '%s'\n", mode_str);
    fprintf(stderr, "Valid modes: manual, fast_attack, slow_attack, hybrid\n");
    exit(1);
}

static const char *agc_mode_to_string(uint8_t mode) {
    if (mode <= RF_GAIN_HYBRID_AGC)
        return agc_mode_names[mode];
    return "unknown";
}

/* RF Initialization */
/*-------------------*/

static void m2sdr_rf_init(
    uint32_t samplerate,
    int64_t  bandwidth,
    int64_t  rx_freq,
    int64_t  rx_gain,
    uint8_t  agc_mode,
    uint8_t  num_channels
) {
    void *conn = m2sdr_open();

#ifdef CSR_SI5351_BASE
    /* Initialize SI5351 Clocking with internal XO */
    printf("Initializing SI5351 Clocking...\n");
    m2sdr_writel(conn, CSR_SI5351_CONTROL_ADDR,
        SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET));
    m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR,
        si5351_xo_40m_config,
        sizeof(si5351_xo_40m_config)/sizeof(si5351_xo_40m_config[0]));
#endif

    /* Power-up AD9361 */
    printf("Powering up AD9361...\n");
    m2sdr_writel(conn, CSR_AD9361_CONFIG_ADDR, 0b11);

    /* Initialize AD9361 SPI */
    printf("Initializing AD9361 SPI...\n");
    m2sdr_ad9361_spi_init(conn, 1);

    /* Initialize AD9361 RFIC */
    if (num_channels == 2) {
        printf("Initializing AD9361 RFIC (2T2R mode)...\n");
        default_init_param.two_rx_two_tx_mode_enable     = 1;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
        default_init_param.two_t_two_r_timing_enable     = 1;
        m2sdr_writel(conn, CSR_AD9361_PHY_CONTROL_ADDR, 0);  /* 2T2R */
    } else {
        printf("Initializing AD9361 RFIC (1T1R mode)...\n");
        default_init_param.two_rx_two_tx_mode_enable     = 0;
        default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
        default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
        default_init_param.two_t_two_r_timing_enable     = 0;
        m2sdr_writel(conn, CSR_AD9361_PHY_CONTROL_ADDR, 1);  /* 1T1R */
    }

    default_init_param.reference_clk_rate = 40000000;  /* 40 MHz */
    default_init_param.gpio_resetb        = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync          = -1;
    default_init_param.gpio_cal_sw1       = -1;
    default_init_param.gpio_cal_sw2       = -1;

    ad9361_init(&ad9361_phy, &default_init_param, 1);

    /* Configure AD9361 Samplerate */
    printf("Setting RX Samplerate to %.2f MSPS.\n", samplerate/1e6);
    ad9361_set_rx_sampling_freq(ad9361_phy, samplerate);

    /* Configure AD9361 RX Bandwidth */
    printf("Setting RX Bandwidth to %.2f MHz.\n", bandwidth/1e6);
    ad9361_set_rx_rf_bandwidth(ad9361_phy, bandwidth);

    /* Configure AD9361 RX Frequency */
    printf("Setting RX LO Freq to %.2f MHz.\n", rx_freq/1e6);
    ad9361_set_rx_lo_freq(ad9361_phy, rx_freq);

    /* Configure AD9361 RX FIR */
    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    /* Configure AD9361 RX Gain Control Mode */
    printf("Setting RX AGC mode to %s.\n", agc_mode_to_string(agc_mode));
    ad9361_set_rx_gain_control_mode(ad9361_phy, 0, agc_mode);
    if (num_channels == 2) {
        ad9361_set_rx_gain_control_mode(ad9361_phy, 1, agc_mode);
    }

    /* Configure AD9361 RX Gain (same for both channels) */
    printf("Setting RX Gain to %ld dB%s.\n", rx_gain,
           agc_mode != RF_GAIN_MGC ? " (initial, AGC will adjust)" : "");
    ad9361_set_rx_rf_gain(ad9361_phy, 0, rx_gain);
    if (num_channels == 2) {
        ad9361_set_rx_rf_gain(ad9361_phy, 1, rx_gain);
    }

    /* 16-bit mode (12-bit samples in 16-bit words) */
    m2sdr_writel(conn, CSR_AD9361_BITMODE_ADDR, 0);

    /* Enable Synchronizer (disable bypass) - matches SoapySDR */
    m2sdr_writel(conn, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 0);

    /* Disable DMA loopback - matches SoapySDR */
    m2sdr_writel(conn, CSR_PCIE_DMA0_LOOPBACK_ENABLE_ADDR, 0);

    m2sdr_close(conn);
}

/* Stream to Shared Memory */
/*-------------------------*/

static void m2sdr_stream_to_shm(
    const char *device_name,
    shm_ring_buffer_t *shm,
    size_t num_samples,  /* 0 for infinite */
    uint8_t quiet
) {
    static struct litepcie_dma_ctrl dma = {.use_writer = 1};

    int i = 0;
    size_t samples_written = 0;
    int64_t last_time;
    int64_t writer_sw_count_last = 0;
    uint64_t chunks_written = 0;
    uint64_t overflow_count = 0;
    uint64_t lost_samples = 0;
    uint64_t buffer_full_count = 0;
    uint64_t pending_lost_samples = 0;  /* Samples lost since last written chunk */

    /* Samples per DMA buffer (accounting for all channels) */
    size_t samples_per_dma = DMA_BUFFER_SIZE / (SAMPLE_SIZE_COMPLEX_INT16 * shm->num_channels);
    /* int16 values per DMA buffer (I and Q for all channels) */
    size_t int16_per_dma = DMA_BUFFER_SIZE / sizeof(int16_t);

    /* Initialize DMA */
    if (litepcie_dma_init(&dma, device_name, 0))  /* zero_copy = 0 */
        exit(1);
    dma.writer_enable = 1;

    /* Configure RX Header (disabled - we just want raw samples) */
    m2sdr_writel(dma.fds.fd, CSR_HEADER_RX_CONTROL_ADDR,
       (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
       (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)
    );

    /* Crossbar Demux: select PCIe streaming for RX */
    m2sdr_writel(dma.fds.fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    /* Reset DMA counters to ensure clean state (matches SoapySDR behavior).
     * This prevents stale counter values from previous runs causing issues.
     * Call with enable=0 to reset counters before starting. */
    litepcie_dma_writer(dma.fds.fd, 0, &dma.writer_hw_count, &dma.writer_sw_count);

    printf("Starting DMA stream to shared memory...\n");
    printf("  DMA buffer size: %d bytes (%zu samples/channel)\n",
           DMA_BUFFER_SIZE, samples_per_dma);
    printf("  Per-chunk header: %d bytes (samples_lost_before)\n", CHUNK_HEADER_SIZE);

    last_time = get_time_ms();

    for (;;) {
        if (!keep_running)
            break;

        if (num_samples > 0 && samples_written >= num_samples)
            break;

        /* Update DMA status */
        litepcie_dma_process(&dma);

        /* Check for DMA overflow BEFORE processing buffers
         * This ensures we know how many samples were lost before each chunk
         */
        if (dma.writer_hw_count > dma.writer_sw_count + 16) {
            uint64_t lost_buffers = dma.writer_hw_count - dma.writer_sw_count - 1;
            uint64_t newly_lost = lost_buffers * samples_per_dma;
            overflow_count++;
            lost_samples += newly_lost;
            pending_lost_samples += newly_lost;
            shm_set_overflow_count(shm, overflow_count);
            shm_set_lost_samples(shm, lost_samples);

            if (!quiet) {
                fprintf(stderr, "\rOVERFLOW #%" PRIu64 ": lost %" PRIu64 " buffers (%" PRIu64 " samples total)\n",
                    overflow_count, lost_buffers, lost_samples);
            }
        }

        /* Read from DMA and copy directly to shared memory */
        while (1) {
            char *buf_rd = litepcie_dma_next_read_buffer(&dma);
            if (!buf_rd)
                break;

            /* Wait for space in ring buffer */
            while (!shm_can_write(shm)) {
                buffer_full_count++;
                usleep(10);
                if (!keep_running)
                    goto done;
            }

            /* Get slot pointer and write per-chunk header first */
            uint64_t write_idx = shm_get_write_index(shm);
            uint8_t *slot = shm_slot_ptr(shm, write_idx);
            shm_set_chunk_lost_samples(slot, pending_lost_samples);

            /* Copy DMA buffer to sample data area (after header) */
            uint8_t *dst = slot + CHUNK_HEADER_SIZE;
            memcpy(dst, buf_rd, DMA_BUFFER_SIZE);

            /* Sign-extend 12-bit samples to 16-bit in-place */
            sign_extend_12bit_inplace((int16_t *)dst, int16_per_dma);

            /* Reset pending lost samples - this chunk now "owns" them */
            pending_lost_samples = 0;

            /* Update counters */
            chunks_written++;
            samples_written += samples_per_dma;
            shm_set_write_index(shm, write_idx + 1);
            shm_set_chunks_written(shm, chunks_written);
        }

        /* Statistics every 200ms */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            double speed = (double)(dma.writer_sw_count - writer_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6);
            uint64_t size_mb = (samples_written * SAMPLE_SIZE_COMPLEX_INT16 * shm->num_channels) / 1024 / 1024;

            if (i % 10 == 0) {
                fprintf(stderr, "\e[1m%10s %12s %9s %10s %12s\e[0m\n",
                    "SPEED(Gbps)", "CHUNKS", "SIZE(MB)", "OVERFLOWS", "BUF_FULL");
            }
            i++;

            fprintf(stderr, "%11.2f %12" PRIu64 " %9" PRIu64 " %10" PRIu64 " %12" PRIu64 "\n",
                speed, chunks_written, size_mb, overflow_count, buffer_full_count);

            last_time = get_time_ms();
            writer_sw_count_last = dma.writer_sw_count;
        }
    }

done:
    litepcie_dma_cleanup(&dma);

    printf("\nStream complete:\n");
    printf("  Chunks written: %" PRIu64 "\n", chunks_written);
    printf("  Samples written: %zu\n", samples_written);
    printf("  Overflows: %" PRIu64 "\n", overflow_count);
    printf("  Lost samples: %" PRIu64 "\n", lost_samples);
    printf("  Buffer full events: %" PRIu64 "\n", buffer_full_count);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Stream to Shared Memory Utility\n"
           "usage: m2sdr_stream_shm [options]\n"
           "\n"
           "Options:\n"
           "  -h                     Show this help message and exit.\n"
           "  -c device_num          Select the device (default: 0).\n"
           "  -q                     Quiet mode (suppress statistics).\n"
           "\n"
           "RF Configuration:\n"
           "  -samplerate sps        Set RF samplerate in Hz (default: %d).\n"
           "  -bandwidth bw          Set the RF bandwidth in Hz (default: %d).\n"
           "  -rx_freq freq          Set the RX frequency in Hz (default: %" PRId64 ").\n"
           "  -rx_gain gain          Set the RX gain in dB (default: %d).\n"
           "  -agc_mode mode         Set AGC mode (default: manual).\n"
           "                         Modes: manual, fast_attack, slow_attack, hybrid\n"
           "  -channels n            Number of RX channels: 1 or 2 (default: 1).\n"
           "\n"
           "Shared Memory Configuration:\n"
           "  -shm_path path         Shared memory path (default: /dev/shm/sdr_ringbuffer).\n"
           "  -buffer_time seconds   Ring buffer duration in seconds (default: 3.0).\n"
           "  -num_samples count     Number of samples to capture (default: 0 = infinite).\n",
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_RX_FREQ,
           DEFAULT_RX_GAIN);
    exit(1);
}

/* Options */
/*---------*/

static struct option options[] = {
    { "help",        no_argument,       NULL, 'h' },
    { "samplerate",  required_argument, NULL, 's' },
    { "bandwidth",   required_argument, NULL, 'b' },
    { "rx_freq",     required_argument, NULL, 'f' },
    { "rx_gain",     required_argument, NULL, 'g' },
    { "agc_mode",    required_argument, NULL, 'a' },
    { "channels",    required_argument, NULL, 'C' },
    { "shm_path",    required_argument, NULL, 'p' },
    { "buffer_time", required_argument, NULL, 't' },
    { "num_samples", required_argument, NULL, 'n' },
    { NULL },
};

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;

    /* Defaults */
    uint32_t samplerate  = DEFAULT_SAMPLERATE;
    int64_t  bandwidth   = DEFAULT_BANDWIDTH;
    int64_t  rx_freq     = DEFAULT_RX_FREQ;
    int64_t  rx_gain     = DEFAULT_RX_GAIN;
    uint8_t  agc_mode    = RF_GAIN_MGC;  /* manual gain control by default */
    uint8_t  num_channels = 1;
    uint8_t  quiet       = 0;

    const char *shm_path = "/dev/shm/sdr_ringbuffer";
    double   buffer_time = 3.0;
    size_t   num_samples = 0;  /* 0 = infinite */

    signal(SIGINT, intHandler);

    /* Parse options */
    for (;;) {
        c = getopt_long_only(argc, argv, "hc:q", options, NULL);
        if (c == -1)
            break;

        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
        case 'q':
            quiet = 1;
            break;
        case 's':
            samplerate = (uint32_t)strtod(optarg, NULL);
            break;
        case 'b':
            bandwidth = (int64_t)strtod(optarg, NULL);
            break;
        case 'f':
            rx_freq = (int64_t)strtod(optarg, NULL);
            break;
        case 'g':
            rx_gain = (int64_t)strtod(optarg, NULL);
            break;
        case 'a':
            agc_mode = parse_agc_mode(optarg);
            break;
        case 'C':
            num_channels = (uint8_t)atoi(optarg);
            if (num_channels != 1 && num_channels != 2) {
                fprintf(stderr, "Error: channels must be 1 or 2\n");
                exit(1);
            }
            break;
        case 'p':
            shm_path = optarg;
            break;
        case 't':
            buffer_time = strtod(optarg, NULL);
            break;
        case 'n':
            num_samples = (size_t)strtoull(optarg, NULL, 0);
            break;
        default:
            exit(1);
        }
    }

    /* Select device */
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);

    printf("M2SDR Stream to Shared Memory\n");
    printf("=============================\n");
    printf("Device: %s\n", m2sdr_device);
    printf("Sample rate: %.2f MHz\n", samplerate / 1e6);
    printf("RX frequency: %.2f MHz\n", rx_freq / 1e6);
    printf("RX gain: %ld dB\n", rx_gain);
    printf("AGC mode: %s\n", agc_mode_to_string(agc_mode));
    printf("Bandwidth: %.2f MHz\n", bandwidth / 1e6);
    printf("Channels: %d (%s)\n", num_channels, num_channels == 2 ? "2T2R" : "1T1R");
    printf("Shared memory: %s\n", shm_path);
    printf("Buffer time: %.1f seconds\n", buffer_time);
    if (num_samples > 0)
        printf("Num samples: %zu\n", num_samples);
    else
        printf("Num samples: infinite (Ctrl+C to stop)\n");
    printf("\n");

    /* Initialize RF */
    m2sdr_rf_init(samplerate, bandwidth, rx_freq, rx_gain, agc_mode, num_channels);

    /* Create shared memory - chunk size = DMA buffer size (no rechunking) */
    shm_ring_buffer_t *shm = shm_create(shm_path, DMA_BUFFER_SIZE,
                                         num_channels, buffer_time, samplerate);
    if (!shm) {
        fprintf(stderr, "Failed to create shared memory\n");
        return 1;
    }

    /* Stream to shared memory */
    m2sdr_stream_to_shm(m2sdr_device, shm, num_samples, quiet);

    /* Cleanup */
    shm_destroy(shm);

    return 0;
}
