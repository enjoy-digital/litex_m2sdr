/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR RX Stream to Shared Memory Utility.
 *
 * This utility configures the AD9361 RF frontend and streams RX data
 * to a shared memory ring buffer. Designed for Julia integration where
 * a larger buffer helps overcome GC pauses.
 *
 * Key features:
 *   - Follows SoapySDR driver DMA patterns exactly for correctness
 *   - No per-chunk lost samples header - pure sample data only
 *   - Simplified overflow detection matching SoapySDR
 *
 * Data Format:
 *   - Samples are Complex{Int16} (4 bytes per sample per channel)
 *   - Multi-channel data is interleaved: I0,Q0,I1,Q1,I0,Q0,I1,Q1,...
 *   - 12-bit ADC samples are sign-extended to 16-bit
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <poll.h>

#include "m2sdr_shm.h"
#include "m2sdr_common.h"

/*
 * Global state
 */
static char g_device_path[256];
static int g_device_num = 0;

/*
 * RF Initialization (RX-specific)
 */
static void rf_init(int fd, uint32_t samplerate, int64_t bandwidth,
                    int64_t rx_freq, int64_t rx_gain, uint8_t agc_mode,
                    uint8_t num_channels)
{
    /* Common AD9361 setup (SI5351, power-up, SPI, mode, bitmode, sync, loopback) */
    m2sdr_rf_common_init(fd, num_channels);

    /* Set synthesizer frequency hint before init */
    default_init_param.rx_synthesizer_frequency_hz = rx_freq;
    default_init_param.rf_rx_bandwidth_hz          = bandwidth;

    ad9361_init(&ad9361_phy, &default_init_param, 1);

    printf("Setting RX sample rate to %.2f MSPS...\n", samplerate / 1e6);
    ad9361_set_rx_sampling_freq(ad9361_phy, samplerate);

    printf("Setting RX bandwidth to %.2f MHz...\n", bandwidth / 1e6);
    ad9361_set_rx_rf_bandwidth(ad9361_phy, bandwidth);

    printf("Setting RX LO frequency to %.2f MHz...\n", rx_freq / 1e6);
    ad9361_set_rx_lo_freq(ad9361_phy, rx_freq);

    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    printf("Setting RX AGC mode to %s...\n", agc_mode_str(agc_mode));
    ad9361_set_rx_gain_control_mode(ad9361_phy, 0, agc_mode);
    if (num_channels == 2)
        ad9361_set_rx_gain_control_mode(ad9361_phy, 1, agc_mode);

    printf("Setting RX gain to %ld dB%s...\n", rx_gain,
           agc_mode != RF_GAIN_MGC ? " (initial, AGC active)" : "");
    ad9361_set_rx_rf_gain(ad9361_phy, 0, rx_gain);
    if (num_channels == 2)
        ad9361_set_rx_rf_gain(ad9361_phy, 1, rx_gain);
}

/*
 * Sign-extend 12-bit samples to 16-bit in-place
 *
 * The AD9361 outputs 12-bit signed samples in 16-bit words.
 * The 12-bit value occupies bits 0-11, with bit 11 as sign.
 * We sign-extend by shifting left 4, then arithmetic right 4.
 */
static inline void sign_extend_12bit_buffer(int16_t *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = (int16_t)(data[i] << 4) >> 4;
    }
}

/*
 * DMA RX Streaming to Shared Memory
 *
 * This follows the SoapySDR acquireReadBuffer/releaseReadBuffer pattern:
 * 1. Check hw_count - user_count for available buffers
 * 2. If none, poll/wait then refresh counters
 * 3. Detect overflow when (hw_count - sw_count) > buf_count/2
 * 4. On overflow, drain all buffers and report
 * 5. Otherwise, process buffer and advance user_count
 * 6. When done with buffer, update sw_count via ioctl
 */
static void stream_to_shm(shm_buffer_t *shm, size_t num_samples, uint8_t quiet)
{
    struct litepcie_dma_ctrl dma;
    memset(&dma, 0, sizeof(dma));

    /* Open device */
    int fd = open(g_device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s\n", g_device_path);
        return;
    }

    /* Setup DMA for RX (writer from FPGA perspective, reader from SW) */
    dma.fds.fd = fd;
    dma.use_writer = 1;
    dma.use_reader = 0;
    dma.loopback = 0;
    dma.zero_copy = 1;
    dma.shared_fd = 1;  /* We already opened fd */

    if (litepcie_dma_init(&dma, "", 1) < 0) {
        fprintf(stderr, "DMA init failed\n");
        close(fd);
        return;
    }

    /* Get DMA buffer info */
    struct litepcie_ioctl_mmap_dma_info dma_info;
    if (ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_INFO, &dma_info) < 0) {
        perror("LITEPCIE_IOCTL_MMAP_DMA_INFO");
        litepcie_dma_cleanup(&dma);
        close(fd);
        return;
    }

    uint32_t dma_buf_size = dma_info.dma_rx_buf_size;
    uint32_t dma_buf_count = dma_info.dma_rx_buf_count;

    /* Samples per DMA buffer (per channel) */
    size_t samples_per_dma = dma_buf_size / (SHM_BYTES_PER_COMPLEX * shm->num_channels);
    /* int16 values per DMA buffer (I and Q for all channels) */
    size_t int16_per_dma = dma_buf_size / sizeof(int16_t);

    printf("DMA configuration:\n");
    printf("  Buffer size: %u bytes (%zu samples/channel)\n", dma_buf_size, samples_per_dma);
    printf("  Buffer count: %u\n", dma_buf_count);

    /* Configure RX header (disabled - raw samples only) */
    m2sdr_writel((void *)(intptr_t)fd, CSR_HEADER_RX_CONTROL_ADDR,
        (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
        (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET));

    /* Crossbar demux: select PCIe for RX */
    m2sdr_writel((void *)(intptr_t)fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    /* Reset DMA counters before starting (SoapySDR pattern) */
    int64_t hw_count = 0, sw_count = 0;
    litepcie_dma_writer(fd, 0, &hw_count, &sw_count);

    /* Our user-space tracking counter (matches SoapySDR user_count) */
    int64_t user_count = 0;

    printf("Starting DMA streaming...\n");

    /* Enable DMA */
    litepcie_dma_writer(fd, 1, &hw_count, &sw_count);

    /* Statistics */
    uint64_t shm_write_idx = 0;
    uint64_t chunks_written = 0;
    uint64_t overflow_count = 0;
    uint64_t buffer_full_waits = 0;
    size_t total_samples = 0;
    int64_t last_stats_time = get_time_ms();
    int64_t last_sw_count = 0;
    int stats_line = 0;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (g_keep_running) {
        if (num_samples > 0 && total_samples >= num_samples)
            break;

        /* Get current DMA counters */
        litepcie_dma_writer(fd, 1, &hw_count, &sw_count);

        /* Check for available buffers */
        int64_t buffers_available = hw_count - user_count;

        if (buffers_available <= 0) {
            /* No buffers, wait for data */
            int ret = poll(&pfd, 1, 100);
            if (ret < 0) {
                if (g_keep_running)
                    perror("poll");
                break;
            }
            if (ret == 0)
                continue;  /* Timeout */

            /* Refresh counters */
            litepcie_dma_writer(fd, 1, &hw_count, &sw_count);
            buffers_available = hw_count - user_count;
            if (buffers_available <= 0)
                continue;
        }

        /*
         * Overflow detection (SoapySDR pattern):
         * If hw is too far ahead of sw, we've lost buffers.
         * Threshold is half the ring buffer.
         */
        if ((hw_count - sw_count) > (int64_t)(dma_buf_count / 2)) {
            overflow_count++;
            shm_store_error_count(shm, overflow_count);

            if (!quiet) {
                fprintf(stderr, "\nOVERFLOW #%" PRIu64 ": hw=%" PRId64 " sw=%" PRId64
                        " (gap=%" PRId64 ", threshold=%u)\n",
                        overflow_count, hw_count, sw_count,
                        hw_count - sw_count, dma_buf_count / 2);
            }

            /* Drain all buffers (SoapySDR recovery pattern) */
            struct litepcie_ioctl_mmap_dma_update update;
            update.sw_count = hw_count;
            ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &update);
            user_count = hw_count;
            sw_count = hw_count;
            continue;
        }

        /* Process available buffers */
        while (buffers_available > 0 && g_keep_running) {
            /* Wait for SHM space */
            while (!shm_can_write(shm, shm_write_idx)) {
                buffer_full_waits++;
                shm_store_buffer_stall(shm, buffer_full_waits);
                usleep(10);
                if (!g_keep_running)
                    goto done;

                /* Check for DMA overflow while waiting */
                litepcie_dma_writer(fd, 1, &hw_count, &sw_count);
                if ((hw_count - sw_count) > (int64_t)(dma_buf_count / 2)) {
                    overflow_count++;
                    shm_store_error_count(shm, overflow_count);
                    if (!quiet) {
                        fprintf(stderr, "\nOVERFLOW #%" PRIu64 " (while waiting for SHM)\n",
                                overflow_count);
                    }
                    /* Drain and restart outer loop */
                    struct litepcie_ioctl_mmap_dma_update update;
                    update.sw_count = hw_count;
                    ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &update);
                    user_count = hw_count;
                    sw_count = hw_count;
                    buffers_available = 0;
                    break;
                }
            }

            if (buffers_available == 0)
                break;

            /* Get DMA buffer pointer */
            int buf_idx = user_count % dma_buf_count;
            char *dma_buf = dma.buf_rd + buf_idx * dma_buf_size;

            /* Get SHM slot pointer */
            uint8_t *shm_slot = shm_slot_ptr(shm, shm_write_idx);

            /* Copy and sign-extend */
            memcpy(shm_slot, dma_buf, dma_buf_size);
            sign_extend_12bit_buffer((int16_t *)shm_slot, int16_per_dma);

            /* Advance user counter (buffer consumed from user perspective) */
            user_count++;
            buffers_available--;

            /* Release buffer to kernel */
            struct litepcie_ioctl_mmap_dma_update update;
            update.sw_count = user_count;
            ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &update);

            /* Update SHM write index (makes data visible to consumer) */
            shm_write_idx++;
            shm_store_write_index(shm, shm_write_idx);

            chunks_written++;
            total_samples += samples_per_dma;

            if (num_samples > 0 && total_samples >= num_samples)
                break;
        }

        /* Statistics display */
        int64_t now = get_time_ms();
        int64_t elapsed = now - last_stats_time;
        if (!quiet && elapsed >= 200) {
            double speed_gbps = (double)(user_count - last_sw_count) * dma_buf_size * 8 / (elapsed * 1e6);
            uint64_t size_mb = (total_samples * SHM_BYTES_PER_COMPLEX * shm->num_channels) / (1024 * 1024);

            if (stats_line % 10 == 0) {
                fprintf(stderr, "\n\033[1m%10s %12s %9s %10s %12s\033[0m\n",
                        "SPEED(Gbps)", "CHUNKS", "SIZE(MB)", "OVERFLOWS", "SHM_WAITS");
            }
            stats_line++;

            fprintf(stderr, "%10.2f %12" PRIu64 " %9" PRIu64 " %10" PRIu64 " %12" PRIu64 "\n",
                    speed_gbps, chunks_written, size_mb, overflow_count, buffer_full_waits);

            last_stats_time = now;
            last_sw_count = user_count;
        }
    }

done:
    /* Disable DMA */
    litepcie_dma_writer(fd, 0, &hw_count, &sw_count);
    litepcie_dma_cleanup(&dma);
    close(fd);

    printf("\nStream complete:\n");
    printf("  Chunks written: %" PRIu64 "\n", chunks_written);
    printf("  Total samples: %zu\n", total_samples);
    printf("  Overflows: %" PRIu64 "\n", overflow_count);
    printf("  SHM buffer-full waits: %" PRIu64 "\n", buffer_full_waits);
}

/*
 * Help
 */
static void print_help(void)
{
    printf("M2SDR RX Stream to Shared Memory\n"
           "Usage: m2sdr_rx_stream_shm [options]\n"
           "\n"
           "Options:\n"
           "  -h                     Show this help\n"
           "  -c device_num          Select device (default: 0)\n"
           "  -q                     Quiet mode\n"
           "\n"
           "RF Configuration:\n"
           "  -samplerate Hz         Sample rate (default: %d)\n"
           "  -bandwidth Hz          RF bandwidth (default: %d)\n"
           "  -rx_freq Hz            RX frequency (default: %" PRId64 ")\n"
           "  -rx_gain dB            RX gain (default: %d)\n"
           "  -agc_mode mode         AGC mode: manual, fast_attack, slow_attack, hybrid\n"
           "  -channels N            Number of channels: 1 or 2 (default: 1)\n"
           "\n"
           "Shared Memory:\n"
           "  -shm_path path         SHM path (default: /dev/shm/sdr_ringbuffer)\n"
           "  -buffer_time seconds   Buffer duration (default: 3.0)\n"
           "  -num_samples N         Samples to capture (default: 0 = infinite)\n",
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_RX_FREQ,
           DEFAULT_RX_GAIN);
    exit(0);
}

static struct option long_options[] = {
    {"help",        no_argument,       NULL, 'h'},
    {"samplerate",  required_argument, NULL, 's'},
    {"bandwidth",   required_argument, NULL, 'b'},
    {"rx_freq",     required_argument, NULL, 'f'},
    {"rx_gain",     required_argument, NULL, 'g'},
    {"agc_mode",    required_argument, NULL, 'a'},
    {"channels",    required_argument, NULL, 'C'},
    {"shm_path",    required_argument, NULL, 'p'},
    {"buffer_time", required_argument, NULL, 't'},
    {"num_samples", required_argument, NULL, 'n'},
    {NULL, 0, NULL, 0}
};

int main(int argc, char **argv)
{
    /* Defaults */
    uint32_t samplerate = DEFAULT_SAMPLERATE;
    int64_t bandwidth = DEFAULT_BANDWIDTH;
    int64_t rx_freq = DEFAULT_RX_FREQ;
    int64_t rx_gain = DEFAULT_RX_GAIN;
    uint8_t agc_mode = RF_GAIN_MGC;
    uint8_t num_channels = 1;
    uint8_t quiet = 0;

    const char *shm_path = "/dev/shm/sdr_ringbuffer";
    double buffer_time = 3.0;
    size_t num_samples = 0;

    m2sdr_install_signal_handlers();

    int c;
    while ((c = getopt_long_only(argc, argv, "hc:q", long_options, NULL)) != -1) {
        switch (c) {
        case 'h':
            print_help();
            break;
        case 'c':
            g_device_num = atoi(optarg);
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
                fprintf(stderr, "Channels must be 1 or 2\n");
                return 1;
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
            return 1;
        }
    }

    snprintf(g_device_path, sizeof(g_device_path), "/dev/m2sdr%d", g_device_num);

    printf("M2SDR RX Stream to Shared Memory\n");
    printf("=================================\n");
    printf("Device: %s\n", g_device_path);
    printf("Sample rate: %.2f MHz\n", samplerate / 1e6);
    printf("Bandwidth: %.2f MHz\n", bandwidth / 1e6);
    printf("RX frequency: %.2f MHz\n", rx_freq / 1e6);
    printf("RX gain: %ld dB\n", rx_gain);
    printf("AGC mode: %s\n", agc_mode_str(agc_mode));
    printf("Channels: %d (%s)\n", num_channels, num_channels == 2 ? "2T2R" : "1T1R");
    printf("SHM path: %s\n", shm_path);
    printf("Buffer time: %.1f seconds\n", buffer_time);
    if (num_samples > 0)
        printf("Num samples: %zu\n", num_samples);
    else
        printf("Num samples: infinite (Ctrl+C to stop)\n");
    printf("\n");

    /* Open device for RF init */
    int fd = open(g_device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s\n", g_device_path);
        return 1;
    }

    /* Initialize RF */
    rf_init(fd, samplerate, bandwidth, rx_freq, rx_gain, agc_mode, num_channels);
    close(fd);

    /* Create shared memory (chunk_bytes = DMA buffer size) */
    shm_buffer_t *shm = shm_create(shm_path, DMA_BUFFER_SIZE, num_channels,
                                    buffer_time, samplerate);
    if (!shm) {
        fprintf(stderr, "Failed to create shared memory\n");
        return 1;
    }

    /* Stream to shared memory */
    stream_to_shm(shm, num_samples, quiet);

    /* Cleanup */
    shm_destroy(shm);

    return 0;
}
