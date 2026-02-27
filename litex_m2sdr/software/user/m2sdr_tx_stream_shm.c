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
 * RF Initialization (TX-specific)
 */
static void rf_init(int fd, uint32_t samplerate, int64_t bandwidth,
                    int64_t tx_freq, int64_t tx_gain, uint8_t num_channels)
{
    /* Common AD9361 setup (SI5351, power-up, SPI, mode, bitmode, sync, loopback) */
    m2sdr_rf_common_init(fd, num_channels);

    /* Set synthesizer frequency hint before init */
    default_init_param.tx_synthesizer_frequency_hz = tx_freq;
    default_init_param.rf_tx_bandwidth_hz          = bandwidth;

    ad9361_init(&ad9361_phy, &default_init_param, 1);

    /* Configure channel mode to match SoapySDR setupStream() behavior.
     * This sets up the port control and calls ad9361_set_no_ch_mode() which
     * performs a full AD9361 reconfiguration including calibrations.
     * Without this, TX signal quality may be degraded. */
    printf("Configuring AD9361 channel mode...\n");

    m2sdr_writel((void *)(intptr_t)fd, CSR_AD9361_PHY_CONTROL_ADDR, num_channels == 1 ? 1 : 0);

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
    printf("Setting TX Samplerate to %.2f MSPS.\n", samplerate / 1e6);

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
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    }

    ad9361_set_tx_sampling_freq(ad9361_phy, samplerate);

    printf("Setting TX Bandwidth to %.2f MHz.\n", bandwidth / 1e6);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, bandwidth);

    printf("Setting TX LO Freq to %.2f MHz.\n", tx_freq / 1e6);
    ad9361_set_tx_lo_freq(ad9361_phy, tx_freq);

    /* Configure TX Attenuation (tx_gain is negative dB attenuation) */
    int32_t attenuation_mdb = (int32_t)(-tx_gain * 1000);
    printf("Setting TX Attenuation to %ld dB (%" PRId32 " mdB).\n", -tx_gain, attenuation_mdb);
    ad9361_set_tx_attenuation(ad9361_phy, 0, attenuation_mdb);
    if (num_channels == 2)
        ad9361_set_tx_attenuation(ad9361_phy, 1, attenuation_mdb);

    /* Configure TX DMA header control - matches SoapySDR setupStream() behavior */
    m2sdr_writel((void *)(intptr_t)fd, CSR_HEADER_TX_CONTROL_ADDR,
        (1 << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET) |
        (0 << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET));
}

/*
 * DMA TX Streaming from Shared Memory
 */
static void play_from_shm(shm_buffer_t *shm, size_t num_samples, uint8_t quiet)
{
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    size_t samples_transmitted = 0;
    int64_t last_time;
    int64_t reader_hw_count_last = 0;
    uint64_t chunks_read = 0;
    uint64_t underflow_count = 0;
    uint64_t buffer_empty_count = 0;

    /* Samples per DMA buffer (accounting for all channels) */
    size_t samples_per_dma = DMA_BUFFER_SIZE / (SHM_BYTES_PER_COMPLEX * shm->num_channels);

    /* Initialize DMA in zero-copy mode */
    if (litepcie_dma_init(&dma, g_device_path, 1)) {
        fprintf(stderr, "litepcie_dma_init failed\n");
        return;
    }
    dma.reader_enable = 1;

    /* Get DMA buffer info */
    uint32_t dma_buf_size = dma.mmap_dma_info.dma_tx_buf_size;
    uint32_t dma_buf_count = dma.mmap_dma_info.dma_tx_buf_count;

    if (dma.buf_wr == NULL || dma.buf_wr == MAP_FAILED) {
        fprintf(stderr, "DMA TX buffer not mapped correctly\n");
        litepcie_dma_cleanup(&dma);
        return;
    }

    if (dma_buf_count == 0 || dma_buf_size == 0) {
        fprintf(stderr, "DMA buffer info not populated (buf_count=%u, buf_size=%u)\n",
                dma_buf_count, dma_buf_size);
        litepcie_dma_cleanup(&dma);
        return;
    }

    /* User-side buffer tracking */
    int64_t user_count = 0;
    int64_t hw_count = 0;

    /* Crossbar Mux: select PCIe streaming for TX */
    m2sdr_writel(dma.fds.fd, CSR_CROSSBAR_MUX_SEL_ADDR, 0);

    /* Reset DMA counters */
    int64_t sw_count_init = 0;
    litepcie_dma_reader(dma.fds.fd, 0, &hw_count, &sw_count_init);
    user_count = hw_count;

    printf("Starting DMA play from shared memory (zero-copy mode)...\n");
    printf("  DMA buffer size: %u bytes (%zu samples/channel)\n",
           dma_buf_size, samples_per_dma);
    printf("  DMA buffer count: %u\n", dma_buf_count);
    printf("  SHM chunk size: %u bytes (%u samples/channel)\n",
           shm->chunk_bytes, shm->chunk_size);

    /* Verify chunk sizes match */
    if (shm->chunk_bytes != dma_buf_size) {
        fprintf(stderr, "Warning: SHM chunk size (%u) != DMA buffer size (%u)\n",
                shm->chunk_bytes, dma_buf_size);
        fprintf(stderr, "         Data will be copied with potential mismatch!\n");
    }

    last_time = get_time_ms();
    int64_t sw_count_tmp = 0;

    for (;;) {
        if (!g_keep_running)
            break;

        if (num_samples > 0 && samples_transmitted >= num_samples)
            break;

        /* Get current HW count */
        litepcie_dma_reader(dma.fds.fd, 1, &hw_count, &sw_count_tmp);

        /* Detect DMA underflow */
        if (hw_count > user_count) {
            underflow_count++;
            shm_store_error_count(shm, underflow_count);
            if (!quiet) {
                fprintf(stderr, "\rUNDERFLOW #%" PRIu64 "\n", underflow_count);
            }
            user_count = hw_count;
        }

        /* Calculate available buffers */
        int64_t buffers_pending = user_count - hw_count;
        int64_t buffers_available = (int64_t)dma_buf_count - buffers_pending;

        /* If no buffers available, poll and wait */
        if (buffers_available <= 0) {
            int ret = poll(&dma.fds, 1, 100);
            if (ret < 0) {
                perror("poll");
                break;
            } else if (ret == 0) {
                continue;
            }
            litepcie_dma_reader(dma.fds.fd, 1, &hw_count, &sw_count_tmp);
            buffers_pending = user_count - hw_count;
            buffers_available = (int64_t)dma_buf_count - buffers_pending;
        }

        /* Fill available DMA buffers from shared memory */
        while (buffers_available > 0) {
            if (!shm_can_read(shm)) {
                /* Check if producer is done */
                if (shm_is_writer_done(shm)) {
                    printf("Producer signaled done.\n");
                    g_keep_running = 0;
                    break;
                }
                /* No data yet. If we have enough headroom, wait */
                int64_t current_pending = user_count - hw_count;
                if (current_pending >= 8) {
                    break;
                }
                /* Running low - send zeros to prevent underflow */
                buffer_empty_count++;
                shm_store_buffer_stall(shm, buffer_empty_count);
                if (!quiet) {
                    fprintf(stderr, "\rBUF_EMPTY #%" PRIu64 " (pending=%" PRId64 ")\n",
                            buffer_empty_count, current_pending);
                }
                int buf_offset = user_count % dma_buf_count;
                char *buf_wr = dma.buf_wr + buf_offset * dma_buf_size;
                memset(buf_wr, 0, dma_buf_size);
            } else {
                /* Copy data from shared memory to DMA buffer */
                int buf_offset = user_count % dma_buf_count;
                char *buf_wr = dma.buf_wr + buf_offset * dma_buf_size;
                uint64_t read_idx = shm_load_read_index(shm);
                uint8_t *src = shm_slot_ptr(shm, read_idx);

                size_t copy_size = (shm->chunk_bytes < dma_buf_size) ?
                                    shm->chunk_bytes : dma_buf_size;
                memcpy(buf_wr, src, copy_size);

                if (copy_size < dma_buf_size) {
                    memset(buf_wr + copy_size, 0, dma_buf_size - copy_size);
                }

                chunks_read++;
                samples_transmitted += shm->chunk_size;
                shm_store_read_index(shm, read_idx + 1);
            }

            /* Memory barrier before submitting to kernel */
            __sync_synchronize();

            /* Advance user count and tell kernel this buffer is ready */
            user_count++;
            struct litepcie_ioctl_mmap_dma_update mmap_dma_update;
            mmap_dma_update.sw_count = user_count;
            checked_ioctl(dma.fds.fd, LITEPCIE_IOCTL_MMAP_DMA_READER_UPDATE, &mmap_dma_update);

            buffers_available--;
        }

        /* Statistics every 200ms */
        int64_t duration = get_time_ms() - last_time;
        if (!quiet && duration > 200) {
            litepcie_dma_reader(dma.fds.fd, 1, &hw_count, &sw_count_tmp);
            double speed = (double)(hw_count - reader_hw_count_last) * dma_buf_size * 8 / ((double)duration * 1e6);
            uint64_t size_mb = (samples_transmitted * SHM_BYTES_PER_COMPLEX * shm->num_channels) / 1024 / 1024;

            if (i % 10 == 0) {
                fprintf(stderr, "\e[1m%10s %12s %9s %10s %12s\e[0m\n",
                    "SPEED(Gbps)", "CHUNKS", "SIZE(MB)", "UNDERFLOWS", "BUF_EMPTY");
            }
            i++;

            fprintf(stderr, "%11.2f %12" PRIu64 " %9" PRIu64 " %10" PRIu64 " %12" PRIu64 "\n",
                speed, chunks_read, size_mb, underflow_count, buffer_empty_count);

            last_time = get_time_ms();
            reader_hw_count_last = hw_count;
        }
    }

    litepcie_dma_cleanup(&dma);

    printf("\nPlay complete:\n");
    printf("  Chunks read: %" PRIu64 "\n", chunks_read);
    printf("  Samples transmitted: %zu\n", samples_transmitted);
    printf("  Underflows: %" PRIu64 "\n", underflow_count);
    printf("  Buffer empty events: %" PRIu64 "\n", buffer_empty_count);
}

/*
 * Help
 */
static void help(void)
{
    printf("M2SDR Play from Shared Memory Utility\n"
           "usage: m2sdr_tx_stream_shm [options]\n"
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

static struct option long_options[] = {
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

int main(int argc, char **argv)
{
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
    size_t   num_samples = 0;

    m2sdr_install_signal_handlers();

    int c;
    for (;;) {
        c = getopt_long_only(argc, argv, "hc:qw", long_options, NULL);
        if (c == -1)
            break;

        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            g_device_num = atoi(optarg);
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

    snprintf(g_device_path, sizeof(g_device_path), "/dev/m2sdr%d", g_device_num);

    printf("M2SDR Play from Shared Memory\n");
    printf("=============================\n");
    printf("Device: %s\n", g_device_path);
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

    /* Open device for RF init */
    int fd = open(g_device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s\n", g_device_path);
        return 1;
    }

    /* Initialize RF */
    rf_init(fd, samplerate, bandwidth, tx_freq, tx_gain, num_channels);
    close(fd);

    /* Open or create shared memory */
    shm_buffer_t *shm = NULL;

    if (wait_for_shm) {
        printf("Waiting for shared memory at %s...\n", shm_path);
        while (!shm && g_keep_running) {
            shm = shm_open_existing(shm_path);
            if (!shm) {
                sleep(1);
            }
        }
        if (!g_keep_running) {
            printf("Interrupted while waiting.\n");
            return 1;
        }
    } else {
        /* Try to open existing, otherwise create new */
        shm = shm_open_existing(shm_path);
        if (!shm) {
            printf("Creating new shared memory...\n");
            shm = shm_create(shm_path, DMA_BUFFER_SIZE, num_channels,
                             buffer_time, samplerate);
        }
    }

    if (!shm) {
        fprintf(stderr, "Failed to open/create shared memory\n");
        return 1;
    }

    /* Play from shared memory */
    play_from_shm(shm, num_samples, quiet);

    /* Cleanup */
    shm_destroy(shm);

    return 0;
}
