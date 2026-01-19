/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Play from Shared Memory Utility.
 *
 * This utility configures the AD9361 RF frontend and streams TX data
 * from a shared memory ring buffer. Designed to run as a separate
 * process from Julia signal processing to avoid GC pause interference.
 *
 * Data Format:
 *   - Each slot contains sample data to be transmitted
 *   - Samples are Complex{Int16} (4 bytes per sample per channel)
 *   - Multi-channel data is interleaved: I0,Q0,I1,Q1,I0,Q0,I1,Q1,...
 *
 * Shared Memory Header Layout (64 bytes):
 *   Bytes 0-7:   write_index (uint64_t) - next slot to write (Julia producer)
 *   Bytes 8-15:  read_index (uint64_t) - next slot to read (C consumer)
 *   Bytes 16-23: chunks_written (uint64_t) - total chunks written by producer
 *   Bytes 24-31: chunks_read (uint64_t) - total chunks read by consumer
 *   Bytes 32-35: chunk_size (uint32_t) - samples per chunk (per channel)
 *   Bytes 36-39: sample_size (uint32_t) - bytes per sample (4 for Complex{Int16})
 *   Bytes 40-43: num_slots (uint32_t) - number of slots in buffer
 *   Bytes 44-45: num_channels (uint16_t) - number of channels (1 or 2)
 *   Bytes 46-47: flags (uint16_t) - bit 0 = writer_done
 *   Bytes 48-55: underflow_count (uint64_t) - number of TX underflows
 *   Bytes 56-63: padding
 *
 * Slot Layout (no per-chunk header for TX - simpler than RX):
 *   [sample data...]
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
#define OFFSET_UNDERFLOW_COUNT  48
#define OFFSET_BUFFER_EMPTY_COUNT 56  /* TX: count of times SHM buffer was empty */

/* Flag bits */
#define FLAG_WRITER_DONE        1

/* Sample size for Complex{Int16} (real + imag, each int16) */
#define SAMPLE_SIZE_COMPLEX_INT16 4

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

static inline uint64_t shm_get_write_index(shm_ring_buffer_t *shm) {
    /* Use atomic load with acquire semantics to ensure we see all data written
     * by the Julia producer before the write_index was updated.
     * The producer issues a release fence before updating write_index. */
    return __atomic_load_n((volatile uint64_t *)(shm->data + OFFSET_WRITE_INDEX), __ATOMIC_ACQUIRE);
}

static inline uint64_t shm_get_read_index(shm_ring_buffer_t *shm) {
    return __atomic_load_n((volatile uint64_t *)(shm->data + OFFSET_READ_INDEX), __ATOMIC_RELAXED);
}

static inline void shm_set_read_index(shm_ring_buffer_t *shm, uint64_t val) {
    /* Use atomic store with release semantics to ensure our memcpy from the slot
     * is complete before the Julia producer sees the updated read_index and
     * potentially overwrites the slot. */
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_READ_INDEX), val, __ATOMIC_RELEASE);
}

static inline void shm_set_chunks_read(shm_ring_buffer_t *shm, uint64_t val) {
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_CHUNKS_READ), val, __ATOMIC_RELAXED);
}

static inline void shm_set_underflow_count(shm_ring_buffer_t *shm, uint64_t val) {
    /* Use atomic store so Julia can safely read this counter */
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_UNDERFLOW_COUNT), val, __ATOMIC_RELAXED);
}

static inline void shm_set_buffer_empty_count(shm_ring_buffer_t *shm, uint64_t val) {
    /* Use atomic store so Julia can safely read this counter */
    __atomic_store_n((volatile uint64_t *)(shm->data + OFFSET_BUFFER_EMPTY_COUNT), val, __ATOMIC_RELAXED);
}

static inline uint16_t shm_get_flags(shm_ring_buffer_t *shm) {
    return *(volatile uint16_t *)(shm->data + OFFSET_FLAGS);
}

static inline bool shm_is_writer_done(shm_ring_buffer_t *shm) {
    return (shm_get_flags(shm) & FLAG_WRITER_DONE) != 0;
}

static inline bool shm_can_read(shm_ring_buffer_t *shm) {
    uint64_t write_idx = shm_get_write_index(shm);
    uint64_t read_idx = shm_get_read_index(shm);
    return read_idx < write_idx;
}

/* Get pointer to sample data for a slot */
static inline uint8_t *shm_slot_ptr(shm_ring_buffer_t *shm, uint64_t slot) {
    size_t offset = SHM_HEADER_SIZE + (slot % shm->num_slots) * shm->chunk_bytes;
    return shm->data + offset;
}

/* Open existing shared memory ring buffer created by Julia producer */
static shm_ring_buffer_t *shm_ringbuf_open(const char *path)
{
    shm_ring_buffer_t *shm = calloc(1, sizeof(shm_ring_buffer_t));
    if (!shm) {
        perror("calloc");
        return NULL;
    }

    /* Open existing shared memory file */
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror(path);
        free(shm);
        return NULL;
    }

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        free(shm);
        return NULL;
    }
    shm->total_size = st.st_size;

    /* Memory map the file */
    shm->data = mmap(NULL, shm->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm->data == MAP_FAILED) {
        perror("mmap");
        free(shm);
        return NULL;
    }

    /* Read header metadata */
    shm->chunk_size = *(uint32_t *)(shm->data + OFFSET_CHUNK_SIZE);
    uint32_t sample_size = *(uint32_t *)(shm->data + OFFSET_SAMPLE_SIZE);
    shm->num_slots = *(uint32_t *)(shm->data + OFFSET_NUM_SLOTS);
    shm->num_channels = *(uint16_t *)(shm->data + OFFSET_NUM_CHANNELS);
    shm->chunk_bytes = shm->chunk_size * sample_size * shm->num_channels;

    printf("Opened shared memory ring buffer:\n");
    printf("  Path: %s\n", path);
    printf("  Chunk size: %u samples/channel (%u bytes)\n", shm->chunk_size, shm->chunk_bytes);
    printf("  Num channels: %u\n", shm->num_channels);
    printf("  Num slots: %u\n", shm->num_slots);
    printf("  Sample size: %u bytes\n", sample_size);
    printf("  Total size: %.1f MB\n", shm->total_size / 1024.0 / 1024.0);

    return shm;
}

/* Create shared memory ring buffer (for when Julia hasn't created it yet) */
static shm_ring_buffer_t *shm_ringbuf_create(const char *path, uint32_t chunk_size,
                                      uint16_t num_channels, double buffer_seconds,
                                      uint32_t sample_rate)
{
    shm_ring_buffer_t *shm = calloc(1, sizeof(shm_ring_buffer_t));
    if (!shm) {
        perror("calloc");
        return NULL;
    }

    shm->num_channels = num_channels;
    shm->chunk_size = chunk_size;
    shm->chunk_bytes = chunk_size * SAMPLE_SIZE_COMPLEX_INT16 * num_channels;

    /* Calculate number of slots for requested buffer time */
    uint64_t bytes_per_second = (uint64_t)sample_rate * SAMPLE_SIZE_COMPLEX_INT16 * num_channels;
    uint64_t total_buffer_bytes = (uint64_t)(bytes_per_second * buffer_seconds);
    shm->num_slots = (total_buffer_bytes + shm->chunk_bytes - 1) / shm->chunk_bytes;
    if (shm->num_slots < 16) shm->num_slots = 16;  /* minimum slots */

    shm->total_size = SHM_HEADER_SIZE + (size_t)shm->num_slots * shm->chunk_bytes;

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
    printf("  Chunk size: %u samples/channel (%u bytes)\n", chunk_size, shm->chunk_bytes);
    printf("  Num channels: %u\n", num_channels);
    printf("  Num slots: %u\n", shm->num_slots);
    printf("  Buffer size: %.1f MB\n", shm->total_size / 1024.0 / 1024.0);
    printf("  Buffer time: %.2f seconds\n",
           (double)shm->num_slots * shm->chunk_size / sample_rate);

    return shm;
}

static void shm_ringbuf_destroy(shm_ring_buffer_t *shm) {
    if (shm) {
        if (shm->data && shm->data != MAP_FAILED) {
            munmap(shm->data, shm->total_size);
        }
        free(shm);
    }
}

/* RF Initialization */
/*-------------------*/

static void m2sdr_rf_init(
    uint32_t samplerate,
    int64_t  bandwidth,
    int64_t  tx_freq,
    int64_t  tx_gain,
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

    /* Configure channel mode to match SoapySDR setupStream() behavior.
     * This sets up the port control and calls ad9361_set_no_ch_mode() which
     * performs a full AD9361 reconfiguration including calibrations.
     * Without this, TX signal quality may be degraded. */
    printf("Configuring AD9361 channel mode...\n");

    /* PHY control register must be set right before ad9361_set_no_ch_mode() */
    m2sdr_writel(conn, CSR_AD9361_PHY_CONTROL_ADDR, num_channels == 1 ? 1 : 0);

    ad9361_phy->pdata->rx2tx2 = (num_channels == 2);
    if (num_channels == 1) {
        ad9361_phy->pdata->rx1tx1_mode_use_tx_num = TX_1;
    } else {
        ad9361_phy->pdata->rx1tx1_mode_use_tx_num = TX_1 | TX_2;
    }
    /* AD9361 Port Control 2t2r timing enable */
    struct ad9361_phy_platform_data *pd = ad9361_phy->pdata;
    pd->port_ctrl.pp_conf[0] &= ~(1 << 2);
    if (num_channels == 2)
        pd->port_ctrl.pp_conf[0] |= (1 << 2);
    ad9361_set_no_ch_mode(ad9361_phy, num_channels);

    /* Configure AD9361 Samplerate with proper FIR interpolation for low rates.
     * This matches SoapySDR's setSampleRate() behavior which is critical for
     * signal quality at low sample rates. */
    printf("Setting TX Samplerate to %.2f MSPS.\n", samplerate/1e6);

    /* For sample rates below 2.5 Msps, enable FIR interpolation.
     * SoapySDR uses interpolation=2 for rates 1.25-2.5 Msps and
     * interpolation=4 for rates < 1.25 Msps. */
    if (samplerate < 1250000) {
        printf("Setting TX FIR Interpolation to 4 (< 1.25 Msps Samplerate).\n");
        ad9361_phy->tx_fir_int    = 4;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config;
        tx_fir_cfg.tx_int = 4;
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    } else if (samplerate < 2500000) {
        printf("Setting TX FIR Interpolation to 2 (< 2.5 Msps Samplerate).\n");
        ad9361_phy->tx_fir_int    = 2;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config;
        tx_fir_cfg.tx_int = 2;
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    } else {
        /* For higher sample rates, just set the default FIR config */
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    }

    ad9361_set_tx_sampling_freq(ad9361_phy, samplerate);

    /* Configure AD9361 TX Bandwidth */
    printf("Setting TX Bandwidth to %.2f MHz.\n", bandwidth/1e6);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, bandwidth);

    /* Configure AD9361 TX Frequency */
    printf("Setting TX LO Freq to %.2f MHz.\n", tx_freq/1e6);
    ad9361_set_tx_lo_freq(ad9361_phy, tx_freq);

    /* Configure AD9361 TX Attenuation (same for both channels)
     * Note: tx_gain is negative dB attenuation (e.g., -10 means 10dB attenuation)
     */
    int32_t attenuation_mdb = (int32_t)(-tx_gain * 1000);  /* Convert to milli-dB */
    printf("Setting TX Attenuation to %ld dB (%" PRId32 " mdB).\n", -tx_gain, attenuation_mdb);
    ad9361_set_tx_attenuation(ad9361_phy, 0, attenuation_mdb);
    if (num_channels == 2) {
        ad9361_set_tx_attenuation(ad9361_phy, 1, attenuation_mdb);
    }

    /* 16-bit mode (samples in 16-bit words) */
    m2sdr_writel(conn, CSR_AD9361_BITMODE_ADDR, 0);

    /* Enable Synchronizer (disable bypass) - matches SoapySDR */
    m2sdr_writel(conn, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 0);

    m2sdr_close(conn);
}

/* Play from Shared Memory */
/*-------------------------*/

static void m2sdr_play_from_shm(
    const char *device_name,
    shm_ring_buffer_t *shm,
    size_t num_samples,  /* 0 for infinite */
    uint8_t quiet
) {
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    size_t samples_transmitted = 0;
    int64_t last_time;
    int64_t reader_sw_count_last = 0;
    uint64_t chunks_read = 0;
    uint64_t underflow_count = 0;
    uint64_t buffer_empty_count = 0;

    /* Samples per DMA buffer (accounting for all channels) */
    size_t samples_per_dma = DMA_BUFFER_SIZE / (SAMPLE_SIZE_COMPLEX_INT16 * shm->num_channels);

    /* Initialize DMA */
    if (litepcie_dma_init(&dma, device_name, 0))  /* zero_copy = 0 */
        exit(1);
    dma.reader_enable = 1;

    /* Crossbar Mux: select PCIe streaming for TX */
    m2sdr_writel(dma.fds.fd, CSR_CROSSBAR_MUX_SEL_ADDR, 0);

    printf("Starting DMA play from shared memory...\n");
    printf("  DMA buffer size: %d bytes (%zu samples/channel)\n",
           DMA_BUFFER_SIZE, samples_per_dma);
    printf("  SHM chunk size: %u bytes (%u samples/channel)\n",
           shm->chunk_bytes, shm->chunk_size);

    /* Verify chunk sizes match */
    if (shm->chunk_bytes != DMA_BUFFER_SIZE) {
        fprintf(stderr, "Warning: SHM chunk size (%u) != DMA buffer size (%d)\n",
                shm->chunk_bytes, DMA_BUFFER_SIZE);
        fprintf(stderr, "         Data will be copied with potential mismatch!\n");
    }

    last_time = get_time_ms();

    for (;;) {
        if (!keep_running)
            break;

        if (num_samples > 0 && samples_transmitted >= num_samples)
            break;

        /* Update DMA status */
        litepcie_dma_process(&dma);

        /* Detect DMA underflow */
        if (dma.reader_sw_count - dma.reader_hw_count < 0) {
            underflow_count++;
            shm_set_underflow_count(shm, underflow_count);
            if (!quiet) {
                fprintf(stderr, "\rUNDERFLOW #%" PRIu64 "\n", underflow_count);
            }
        }

        /* Write to DMA from shared memory */
        while (1) {
            /* Get DMA write buffer */
            char *buf_wr = litepcie_dma_next_write_buffer(&dma);
            if (!buf_wr)
                break;

            /* Check if data available in shared memory */
            if (!shm_can_read(shm)) {
                /* Check if producer is done */
                if (shm_is_writer_done(shm)) {
                    printf("Producer signaled done.\n");
                    keep_running = 0;
                    break;
                }
                /* No data yet - send zeros to maintain continuous TX.
                 * This causes SNR fluctuations but preserves code phase for CDMA tracking.
                 * Track the count so Julia can monitor buffer health. */
                buffer_empty_count++;
                shm_set_buffer_empty_count(shm, buffer_empty_count);
                memset(buf_wr, 0, DMA_BUFFER_SIZE);
            } else {
                /* Copy data from shared memory to DMA buffer */
                uint64_t read_idx = shm_get_read_index(shm);
                uint8_t *src = shm_slot_ptr(shm, read_idx);

                /* Handle size mismatch: copy min of both sizes */
                size_t copy_size = (shm->chunk_bytes < DMA_BUFFER_SIZE) ?
                                    shm->chunk_bytes : DMA_BUFFER_SIZE;
                memcpy(buf_wr, src, copy_size);


                /* Zero-pad if SHM chunk is smaller than DMA buffer */
                if (copy_size < DMA_BUFFER_SIZE) {
                    memset(buf_wr + copy_size, 0, DMA_BUFFER_SIZE - copy_size);
                }

                /* Update read index */
                chunks_read++;
                samples_transmitted += shm->chunk_size;
                shm_set_read_index(shm, read_idx + 1);
                shm_set_chunks_read(shm, chunks_read);
            }
        }

        /* Statistics every 200ms */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            double speed = (double)(dma.reader_sw_count - reader_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6);
            uint64_t size_mb = (samples_transmitted * SAMPLE_SIZE_COMPLEX_INT16 * shm->num_channels) / 1024 / 1024;

            if (i % 10 == 0) {
                fprintf(stderr, "\e[1m%10s %12s %9s %10s %12s\e[0m\n",
                    "SPEED(Gbps)", "CHUNKS", "SIZE(MB)", "UNDERFLOWS", "BUF_EMPTY");
            }
            i++;

            fprintf(stderr, "%11.2f %12" PRIu64 " %9" PRIu64 " %10" PRIu64 " %12" PRIu64 "\n",
                speed, chunks_read, size_mb, underflow_count, buffer_empty_count);

            last_time = get_time_ms();
            reader_sw_count_last = dma.reader_sw_count;
        }
    }

    litepcie_dma_cleanup(&dma);

    printf("\nPlay complete:\n");
    printf("  Chunks read: %" PRIu64 "\n", chunks_read);
    printf("  Samples transmitted: %zu\n", samples_transmitted);
    printf("  Underflows: %" PRIu64 "\n", underflow_count);
    printf("  Buffer empty events: %" PRIu64 "\n", buffer_empty_count);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Play from Shared Memory Utility\n"
           "usage: m2sdr_play_shm [options]\n"
           "\n"
           "Options:\n"
           "  -h                     Show this help message and exit.\n"
           "  -c device_num          Select the device (default: 0).\n"
           "  -q                     Quiet mode (suppress statistics).\n"
           "  -w                     Wait for shared memory to be created.\n"
           "\n"
           "RF Configuration:\n"
           "  -samplerate sps        Set RF samplerate in Hz (default: %d).\n"
           "  -bandwidth bw          Set the RF bandwidth in Hz (default: %d).\n"
           "  -tx_freq freq          Set the TX frequency in Hz (default: %" PRId64 ").\n"
           "  -tx_gain gain          Set the TX gain in dB (default: %d, negative = attenuation).\n"
           "  -channels n            Number of TX channels: 1 or 2 (default: 1).\n"
           "\n"
           "Shared Memory Configuration:\n"
           "  -shm_path path         Shared memory path (default: /dev/shm/sdr_tx_ringbuffer).\n"
           "  -buffer_time seconds   Ring buffer duration if creating (default: 3.0).\n"
           "  -num_samples count     Number of samples to transmit (default: 0 = infinite).\n",
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_TX_FREQ,
           DEFAULT_TX_GAIN);
    exit(1);
}

/* Options */
/*---------*/

static struct option options[] = {
    { "help",        no_argument,       NULL, 'h' },
    { "samplerate",  required_argument, NULL, 's' },
    { "bandwidth",   required_argument, NULL, 'b' },
    { "tx_freq",     required_argument, NULL, 'f' },
    { "tx_gain",     required_argument, NULL, 'g' },
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
    int64_t  tx_freq     = DEFAULT_TX_FREQ;
    int64_t  tx_gain     = DEFAULT_TX_GAIN;
    uint8_t  num_channels = 1;
    uint8_t  quiet       = 0;
    uint8_t  wait_for_shm = 0;

    const char *shm_path = "/dev/shm/sdr_tx_ringbuffer";
    double   buffer_time = 3.0;
    size_t   num_samples = 0;  /* 0 = infinite */

    signal(SIGINT, intHandler);

    /* Parse options */
    for (;;) {
        c = getopt_long_only(argc, argv, "hc:qw", options, NULL);
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
        case 'w':
            wait_for_shm = 1;
            break;
        case 's':
            samplerate = (uint32_t)strtod(optarg, NULL);
            break;
        case 'b':
            bandwidth = (int64_t)strtod(optarg, NULL);
            break;
        case 'f':
            tx_freq = (int64_t)strtod(optarg, NULL);
            break;
        case 'g':
            tx_gain = (int64_t)strtod(optarg, NULL);
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

    printf("M2SDR Play from Shared Memory\n");
    printf("=============================\n");
    printf("Device: %s\n", m2sdr_device);
    printf("Sample rate: %.2f MHz\n", samplerate / 1e6);
    printf("TX frequency: %.2f MHz\n", tx_freq / 1e6);
    printf("TX gain: %ld dB\n", tx_gain);
    printf("Bandwidth: %.2f MHz\n", bandwidth / 1e6);
    printf("Channels: %d (%s)\n", num_channels, num_channels == 2 ? "2T2R" : "1T1R");
    printf("Shared memory: %s\n", shm_path);
    if (num_samples > 0)
        printf("Num samples: %zu\n", num_samples);
    else
        printf("Num samples: infinite (Ctrl+C to stop)\n");
    printf("\n");

    /* Initialize RF */
    m2sdr_rf_init(samplerate, bandwidth, tx_freq, tx_gain, num_channels);

    /* Open or create shared memory */
    shm_ring_buffer_t *shm = NULL;

    if (wait_for_shm) {
        printf("Waiting for shared memory at %s...\n", shm_path);
        while (!shm && keep_running) {
            shm = shm_ringbuf_open(shm_path);
            if (!shm) {
                sleep(1);
            }
        }
        if (!keep_running) {
            printf("Interrupted while waiting.\n");
            return 1;
        }
    } else {
        /* Try to open existing, otherwise create new */
        shm = shm_ringbuf_open(shm_path);
        if (!shm) {
            printf("Creating new shared memory...\n");
            /* Use DMA buffer size as chunk size for simplicity */
            uint32_t chunk_size = DMA_BUFFER_SIZE / (SAMPLE_SIZE_COMPLEX_INT16 * num_channels);
            shm = shm_ringbuf_create(shm_path, chunk_size, num_channels, buffer_time, samplerate);
        }
    }

    if (!shm) {
        fprintf(stderr, "Failed to open/create shared memory\n");
        return 1;
    }

    /* Play from shared memory */
    m2sdr_play_from_shm(m2sdr_device, shm, num_samples, quiet);

    /* Cleanup */
    shm_ringbuf_destroy(shm);

    return 0;
}
