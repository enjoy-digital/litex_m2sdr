/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Full-Duplex Stream Shared Memory Utility.
 *
 * Configures the AD9361 RF frontend and simultaneously streams:
 *   - RX data to a shared memory ring buffer (producer)
 *   - TX data from a separate shared memory ring buffer (consumer)
 *
 * Uses two separate DMA control structs on a shared file descriptor,
 * following the SoapySDR driver pattern for full-duplex operation.
 *
 * Data Format:
 *   - Samples are Complex{Int16} (4 bytes per sample per channel)
 *   - Multi-channel data is interleaved: I0,Q0,I1,Q1,I0,Q0,I1,Q1,...
 *   - 12-bit ADC RX samples are sign-extended to 16-bit
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
 * RF Initialization (Full-Duplex: combined RX + TX)
 *
 * Single ad9361_init() call with both RX and TX frequency hints,
 * followed by direction-specific configuration.
 */
static void rf_init(int fd, uint32_t samplerate, int64_t bandwidth,
                    int64_t rx_freq, int64_t rx_gain, uint8_t agc_mode,
                    int64_t tx_freq, int64_t tx_gain,
                    uint8_t num_channels)
{
    /* Common AD9361 setup (SI5351, power-up, SPI, mode, bitmode, sync, loopback) */
    m2sdr_rf_common_init(fd, num_channels);

    /* Set both RX and TX synthesizer frequency hints before init */
    default_init_param.rx_synthesizer_frequency_hz = rx_freq;
    default_init_param.rf_rx_bandwidth_hz          = bandwidth;
    default_init_param.tx_synthesizer_frequency_hz = tx_freq;
    default_init_param.rf_tx_bandwidth_hz          = bandwidth;

    ad9361_init(&ad9361_phy, &default_init_param, 1);

    /* Configure channel mode (required for TX signal quality) */
    printf("Configuring AD9361 channel mode...\n");
    m2sdr_writel((void *)(intptr_t)fd, CSR_AD9361_PHY_CONTROL_ADDR, num_channels == 1 ? 1 : 0);
    ad9361_phy->pdata->rx2tx2 = (num_channels == 2);
    if (num_channels == 1) {
        ad9361_phy->pdata->rx1tx1_mode_use_tx_num = TX_1;
    } else {
        ad9361_phy->pdata->rx1tx1_mode_use_tx_num = TX_1 | TX_2;
    }
    struct ad9361_phy_platform_data *pd = ad9361_phy->pdata;
    pd->port_ctrl.pp_conf[0] &= ~(1 << 2);
    if (num_channels == 2)
        pd->port_ctrl.pp_conf[0] |= (1 << 2);
    ad9361_set_no_ch_mode(ad9361_phy, num_channels);

    /* --- RX Configuration --- */

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

    /* --- TX Configuration --- */

    printf("Setting TX sample rate to %.2f MSPS...\n", samplerate / 1e6);
    if (samplerate < 1250000) {
        printf("Setting TX FIR Interpolation to 4 (< 1.25 Msps).\n");
        ad9361_phy->tx_fir_int    = 4;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config;
        tx_fir_cfg.tx_int = 4;
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    } else if (samplerate < 2500000) {
        printf("Setting TX FIR Interpolation to 2 (< 2.5 Msps).\n");
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

    printf("Setting TX bandwidth to %.2f MHz...\n", bandwidth / 1e6);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, bandwidth);

    printf("Setting TX LO frequency to %.2f MHz...\n", tx_freq / 1e6);
    ad9361_set_tx_lo_freq(ad9361_phy, tx_freq);

    int32_t attenuation_mdb = (int32_t)(-tx_gain * 1000);
    printf("Setting TX attenuation to %ld dB (%" PRId32 " mdB)...\n", -tx_gain, attenuation_mdb);
    ad9361_set_tx_attenuation(ad9361_phy, 0, attenuation_mdb);
    if (num_channels == 2)
        ad9361_set_tx_attenuation(ad9361_phy, 1, attenuation_mdb);

    /* --- Header and Crossbar Configuration --- */

    /* RX header control (disabled - raw samples only) */
    m2sdr_writel((void *)(intptr_t)fd, CSR_HEADER_RX_CONTROL_ADDR,
        (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
        (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET));

    /* TX header control (disabled) */
    m2sdr_writel((void *)(intptr_t)fd, CSR_HEADER_TX_CONTROL_ADDR,
        (1 << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET) |
        (0 << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET));

    /* Crossbar: PCIe for both RX and TX */
    m2sdr_writel((void *)(intptr_t)fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);
    m2sdr_writel((void *)(intptr_t)fd, CSR_CROSSBAR_MUX_SEL_ADDR, 0);
}

/*
 * Sign-extend 12-bit samples to 16-bit in-place
 */
static inline void sign_extend_12bit_buffer(int16_t *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = (int16_t)(data[i] << 4) >> 4;
    }
}

/*
 * Full-Duplex DMA Streaming
 *
 * Uses two DMA control structs on a shared fd (SoapySDR pattern):
 *   - rx_dma: writer from FPGA perspective (FPGA writes RX data, CPU reads)
 *   - tx_dma: reader from FPGA perspective (CPU writes TX data, FPGA reads)
 *
 * Single-threaded with poll(POLLIN|POLLOUT) multiplexing both directions.
 */
static void stream_duplex(shm_buffer_t *rx_shm, shm_buffer_t *tx_shm,
                          size_t num_samples, uint8_t quiet)
{
    /* Open device */
    int fd = open(g_device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s\n", g_device_path);
        return;
    }

    /* --- RX DMA Setup --- */
    struct litepcie_dma_ctrl rx_dma;
    memset(&rx_dma, 0, sizeof(rx_dma));
    rx_dma.fds.fd     = fd;
    rx_dma.use_writer = 1;
    rx_dma.use_reader = 0;
    rx_dma.loopback   = 0;
    rx_dma.zero_copy  = 1;
    rx_dma.shared_fd  = 1;

    if (litepcie_dma_init(&rx_dma, "", 1) < 0) {
        fprintf(stderr, "RX DMA init failed\n");
        close(fd);
        return;
    }

    /* --- TX DMA Setup --- */
    struct litepcie_dma_ctrl tx_dma;
    memset(&tx_dma, 0, sizeof(tx_dma));
    tx_dma.fds.fd     = fd;
    tx_dma.use_reader = 1;
    tx_dma.use_writer = 0;
    tx_dma.loopback   = 0;
    tx_dma.zero_copy  = 1;
    tx_dma.shared_fd  = 1;

    if (litepcie_dma_init(&tx_dma, "", 1) < 0) {
        fprintf(stderr, "TX DMA init failed\n");
        litepcie_dma_cleanup(&rx_dma);
        close(fd);
        return;
    }

    /* --- DMA Buffer Info --- */
    uint32_t rx_buf_size  = rx_dma.mmap_dma_info.dma_rx_buf_size;
    uint32_t rx_buf_count = rx_dma.mmap_dma_info.dma_rx_buf_count;
    uint32_t tx_buf_size  = tx_dma.mmap_dma_info.dma_tx_buf_size;
    uint32_t tx_buf_count = tx_dma.mmap_dma_info.dma_tx_buf_count;

    size_t rx_samples_per_dma = rx_buf_size / (SHM_BYTES_PER_COMPLEX * rx_shm->num_channels);
    size_t rx_int16_per_dma   = rx_buf_size / sizeof(int16_t);

    if (tx_dma.buf_wr == NULL || tx_dma.buf_wr == MAP_FAILED) {
        fprintf(stderr, "TX DMA buffer not mapped correctly\n");
        litepcie_dma_cleanup(&rx_dma);
        litepcie_dma_cleanup(&tx_dma);
        close(fd);
        return;
    }

    printf("DMA configuration:\n");
    printf("  RX: %u bytes/buf x %u bufs (%zu samples/channel/buf)\n",
           rx_buf_size, rx_buf_count, rx_samples_per_dma);
    printf("  TX: %u bytes/buf x %u bufs\n", tx_buf_size, tx_buf_count);

    /* Verify chunk sizes match DMA buffer sizes */
    if (tx_shm->chunk_bytes != tx_buf_size) {
        fprintf(stderr, "Warning: TX SHM chunk size (%u) != DMA buffer size (%u)\n",
                tx_shm->chunk_bytes, tx_buf_size);
    }

    /* --- Reset and Enable DMA Counters --- */
    int64_t rx_hw_count = 0, rx_sw_count = 0;
    litepcie_dma_writer(fd, 0, &rx_hw_count, &rx_sw_count);
    int64_t rx_user_count = 0;

    int64_t tx_hw_count = 0, tx_sw_count_init = 0;
    litepcie_dma_reader(fd, 0, &tx_hw_count, &tx_sw_count_init);
    int64_t tx_user_count = tx_hw_count;

    printf("Starting full-duplex DMA streaming...\n");

    /* Enable DMA for both directions */
    litepcie_dma_writer(fd, 1, &rx_hw_count, &rx_sw_count);
    litepcie_dma_reader(fd, 1, &tx_hw_count, &tx_sw_count_init);

    /* --- Statistics --- */
    uint64_t rx_shm_write_idx   = 0;
    uint64_t rx_chunks_written  = 0;
    uint64_t rx_overflow_count  = 0;
    uint64_t rx_buffer_full_waits = 0;
    size_t   rx_total_samples   = 0;

    uint64_t tx_chunks_read       = 0;
    uint64_t tx_underflow_count   = 0;
    uint64_t tx_buffer_empty_count = 0;
    size_t   tx_total_samples     = 0;

    int64_t last_stats_time    = get_time_ms();
    int64_t last_rx_user_count = 0;
    int64_t last_tx_hw_count   = 0;
    int stats_line = 0;

    /* --- Main Loop --- */
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;

    while (g_keep_running) {
        if (num_samples > 0 && rx_total_samples >= num_samples)
            break;

        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (g_keep_running)
                perror("poll");
            break;
        }
        if (ret == 0)
            continue;

        /* === RX Processing === */
        if (pfd.revents & POLLIN) {
            litepcie_dma_writer(fd, 1, &rx_hw_count, &rx_sw_count);
            int64_t rx_buffers_available = rx_hw_count - rx_user_count;

            /* Overflow detection */
            if ((rx_hw_count - rx_sw_count) > (int64_t)(rx_buf_count / 2)) {
                rx_overflow_count++;
                shm_store_error_count(rx_shm, rx_overflow_count);
                if (!quiet) {
                    fprintf(stderr, "\nRX OVERFLOW #%" PRIu64 ": hw=%" PRId64 " sw=%" PRId64
                            " (gap=%" PRId64 ")\n",
                            rx_overflow_count, rx_hw_count, rx_sw_count,
                            rx_hw_count - rx_sw_count);
                }
                /* Drain all buffers */
                struct litepcie_ioctl_mmap_dma_update update;
                update.sw_count = rx_hw_count;
                ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &update);
                rx_user_count = rx_hw_count;
                rx_sw_count = rx_hw_count;
                rx_buffers_available = 0;
            }

            /* Process available RX buffers */
            while (rx_buffers_available > 0 && g_keep_running) {
                /* Wait for RX SHM space */
                while (!shm_can_write(rx_shm, rx_shm_write_idx)) {
                    rx_buffer_full_waits++;
                    shm_store_buffer_stall(rx_shm, rx_buffer_full_waits);
                    usleep(10);
                    if (!g_keep_running)
                        goto done;

                    /* Check for overflow while waiting */
                    litepcie_dma_writer(fd, 1, &rx_hw_count, &rx_sw_count);
                    if ((rx_hw_count - rx_sw_count) > (int64_t)(rx_buf_count / 2)) {
                        rx_overflow_count++;
                        shm_store_error_count(rx_shm, rx_overflow_count);
                        if (!quiet)
                            fprintf(stderr, "\nRX OVERFLOW #%" PRIu64 " (while waiting for SHM)\n",
                                    rx_overflow_count);
                        struct litepcie_ioctl_mmap_dma_update update;
                        update.sw_count = rx_hw_count;
                        ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &update);
                        rx_user_count = rx_hw_count;
                        rx_sw_count = rx_hw_count;
                        rx_buffers_available = 0;
                        break;
                    }
                }

                if (rx_buffers_available == 0)
                    break;

                /* Copy DMA buffer to RX SHM slot */
                int rx_buf_idx = rx_user_count % rx_buf_count;
                char *dma_buf = rx_dma.buf_rd + rx_buf_idx * rx_buf_size;
                uint8_t *shm_slot = shm_slot_ptr(rx_shm, rx_shm_write_idx);

                memcpy(shm_slot, dma_buf, rx_buf_size);
                sign_extend_12bit_buffer((int16_t *)shm_slot, rx_int16_per_dma);

                /* Advance counters */
                rx_user_count++;
                rx_buffers_available--;

                struct litepcie_ioctl_mmap_dma_update update;
                update.sw_count = rx_user_count;
                ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &update);

                rx_shm_write_idx++;
                shm_store_write_index(rx_shm, rx_shm_write_idx);

                rx_chunks_written++;
                rx_total_samples += rx_samples_per_dma;

                if (num_samples > 0 && rx_total_samples >= num_samples)
                    break;
            }
        }

        /* === TX Processing === */
        if (pfd.revents & POLLOUT) {
            int64_t tx_sw_count_tmp = 0;
            litepcie_dma_reader(fd, 1, &tx_hw_count, &tx_sw_count_tmp);

            /* Underflow detection */
            if (tx_hw_count > tx_user_count) {
                tx_underflow_count++;
                shm_store_error_count(tx_shm, tx_underflow_count);
                if (!quiet)
                    fprintf(stderr, "\nTX UNDERFLOW #%" PRIu64 "\n", tx_underflow_count);
                tx_user_count = tx_hw_count;
            }

            /* Calculate available TX DMA buffers */
            int64_t tx_buffers_pending = tx_user_count - tx_hw_count;
            int64_t tx_buffers_available = (int64_t)tx_buf_count - tx_buffers_pending;

            /* Fill available TX DMA buffers from SHM */
            while (tx_buffers_available > 0) {
                if (!shm_can_read(tx_shm)) {
                    /* Check if producer is done */
                    if (shm_is_writer_done(tx_shm)) {
                        printf("TX producer signaled done.\n");
                        g_keep_running = 0;
                        break;
                    }
                    /* No data yet. If enough headroom, wait */
                    int64_t current_pending = tx_user_count - tx_hw_count;
                    if (current_pending >= 8)
                        break;
                    /* Running low - send zeros to prevent underflow */
                    tx_buffer_empty_count++;
                    shm_store_buffer_stall(tx_shm, tx_buffer_empty_count);
                    if (!quiet)
                        fprintf(stderr, "\nTX BUF_EMPTY #%" PRIu64 " (pending=%" PRId64 ")\n",
                                tx_buffer_empty_count, current_pending);
                    int buf_offset = tx_user_count % tx_buf_count;
                    char *buf_wr = tx_dma.buf_wr + buf_offset * tx_buf_size;
                    memset(buf_wr, 0, tx_buf_size);
                } else {
                    /* Copy data from TX SHM slot to DMA buffer */
                    int buf_offset = tx_user_count % tx_buf_count;
                    char *buf_wr = tx_dma.buf_wr + buf_offset * tx_buf_size;
                    uint64_t read_idx = shm_load_read_index(tx_shm);
                    uint8_t *src = shm_slot_ptr(tx_shm, read_idx);

                    size_t copy_size = (tx_shm->chunk_bytes < tx_buf_size) ?
                                        tx_shm->chunk_bytes : tx_buf_size;
                    memcpy(buf_wr, src, copy_size);
                    if (copy_size < tx_buf_size)
                        memset(buf_wr + copy_size, 0, tx_buf_size - copy_size);

                    tx_chunks_read++;
                    tx_total_samples += tx_shm->chunk_size;
                    shm_store_read_index(tx_shm, read_idx + 1);
                }

                __sync_synchronize();

                tx_user_count++;
                struct litepcie_ioctl_mmap_dma_update mmap_dma_update;
                mmap_dma_update.sw_count = tx_user_count;
                checked_ioctl(fd, LITEPCIE_IOCTL_MMAP_DMA_READER_UPDATE, &mmap_dma_update);

                tx_buffers_available--;
            }
        }

        /* === Statistics Display === */
        int64_t now = get_time_ms();
        int64_t elapsed = now - last_stats_time;
        if (!quiet && elapsed >= 200) {
            double rx_speed = (double)(rx_user_count - last_rx_user_count) * rx_buf_size * 8 / (elapsed * 1e6);
            double tx_speed = (double)(tx_hw_count - last_tx_hw_count) * tx_buf_size * 8 / (elapsed * 1e6);

            if (stats_line % 10 == 0) {
                fprintf(stderr, "\n\033[1m%8s %8s %8s %8s  %8s %8s %8s %8s\033[0m\n",
                        "RX Gbps", "RX_CHKS", "RX_OVFL", "SHM_WAIT",
                        "TX Gbps", "TX_CHKS", "TX_UNFL", "BUF_EMPT");
            }
            stats_line++;

            fprintf(stderr, "%8.2f %8" PRIu64 " %8" PRIu64 " %8" PRIu64
                           "  %8.2f %8" PRIu64 " %8" PRIu64 " %8" PRIu64 "\n",
                    rx_speed, rx_chunks_written, rx_overflow_count, rx_buffer_full_waits,
                    tx_speed, tx_chunks_read, tx_underflow_count, tx_buffer_empty_count);

            last_stats_time = now;
            last_rx_user_count = rx_user_count;
            last_tx_hw_count = tx_hw_count;
        }
    }

done:
    /* Disable DMA */
    litepcie_dma_writer(fd, 0, &rx_hw_count, &rx_sw_count);
    litepcie_dma_reader(fd, 0, &tx_hw_count, &tx_sw_count_init);
    litepcie_dma_cleanup(&rx_dma);
    litepcie_dma_cleanup(&tx_dma);
    close(fd);

    printf("\nFull-duplex stream complete:\n");
    printf("  RX: %" PRIu64 " chunks, %zu samples, %" PRIu64 " overflows, %" PRIu64 " SHM waits\n",
           rx_chunks_written, rx_total_samples, rx_overflow_count, rx_buffer_full_waits);
    printf("  TX: %" PRIu64 " chunks, %zu samples, %" PRIu64 " underflows, %" PRIu64 " buffer empty\n",
           tx_chunks_read, tx_total_samples, tx_underflow_count, tx_buffer_empty_count);
}

/*
 * Help
 */
static void print_help(void)
{
    printf("M2SDR Full-Duplex Stream Shared Memory\n"
           "Usage: m2sdr_stream_shm [options]\n"
           "\n"
           "Options:\n"
           "  -h                         Show this help\n"
           "  -c device_num              Select device (default: 0)\n"
           "  -q                         Quiet mode\n"
           "  -w                         Wait for TX SHM to be created\n"
           "\n"
           "RF Configuration (shared):\n"
           "  -samplerate Hz             Sample rate (default: %d)\n"
           "  -bandwidth Hz              RF bandwidth (default: %d)\n"
           "  -channels N                Number of channels: 1 or 2 (default: 1)\n"
           "\n"
           "RX Configuration:\n"
           "  -rx_freq Hz                RX frequency (default: %" PRId64 ")\n"
           "  -rx_gain dB                RX gain (default: %d)\n"
           "  -agc_mode mode             AGC: manual, fast_attack, slow_attack, hybrid\n"
           "\n"
           "TX Configuration:\n"
           "  -tx_freq Hz                TX frequency (default: %" PRId64 ")\n"
           "  -tx_gain dB                TX gain (default: %d, negative = attenuation)\n"
           "\n"
           "Shared Memory:\n"
           "  -rx_shm_path path          RX SHM path (default: /dev/shm/sdr_ringbuffer)\n"
           "  -tx_shm_path path          TX SHM path (default: /dev/shm/sdr_tx_ringbuffer)\n"
           "  -rx_buffer_time seconds    RX buffer duration (default: 3.0)\n"
           "  -tx_buffer_time seconds    TX buffer duration (default: 3.0)\n"
           "  -num_samples N             Samples to stream (default: 0 = infinite)\n",
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_RX_FREQ,
           DEFAULT_RX_GAIN,
           DEFAULT_TX_FREQ,
           DEFAULT_TX_GAIN);
    exit(0);
}

static struct option long_options[] = {
    {"help",           no_argument,       NULL, 'h'},
    {"samplerate",     required_argument, NULL, 's'},
    {"bandwidth",      required_argument, NULL, 'b'},
    {"rx_freq",        required_argument, NULL, 'R'},
    {"rx_gain",        required_argument, NULL, 'G'},
    {"agc_mode",       required_argument, NULL, 'a'},
    {"tx_freq",        required_argument, NULL, 'T'},
    {"tx_gain",        required_argument, NULL, 'A'},
    {"channels",       required_argument, NULL, 'C'},
    {"rx_shm_path",    required_argument, NULL, 'r'},
    {"tx_shm_path",    required_argument, NULL, 't'},
    {"rx_buffer_time", required_argument, NULL, 'B'},
    {"tx_buffer_time", required_argument, NULL, 'D'},
    {"num_samples",    required_argument, NULL, 'n'},
    {NULL, 0, NULL, 0}
};

int main(int argc, char **argv)
{
    /* Defaults */
    uint32_t samplerate  = DEFAULT_SAMPLERATE;
    int64_t  bandwidth   = DEFAULT_BANDWIDTH;
    int64_t  rx_freq     = DEFAULT_RX_FREQ;
    int64_t  rx_gain     = DEFAULT_RX_GAIN;
    uint8_t  agc_mode    = RF_GAIN_MGC;
    int64_t  tx_freq     = DEFAULT_TX_FREQ;
    int64_t  tx_gain     = DEFAULT_TX_GAIN;
    uint8_t  num_channels = 1;
    uint8_t  quiet       = 0;
    uint8_t  wait_for_tx_shm = 0;

    const char *rx_shm_path = "/dev/shm/sdr_ringbuffer";
    const char *tx_shm_path = "/dev/shm/sdr_tx_ringbuffer";
    double   rx_buffer_time = 3.0;
    double   tx_buffer_time = 3.0;
    size_t   num_samples    = 0;

    m2sdr_install_signal_handlers();

    int c;
    while ((c = getopt_long_only(argc, argv, "hc:qw", long_options, NULL)) != -1) {
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
        case 'w':
            wait_for_tx_shm = 1;
            break;
        case 's':
            samplerate = (uint32_t)strtod(optarg, NULL);
            break;
        case 'b':
            bandwidth = (int64_t)strtod(optarg, NULL);
            break;
        case 'R':
            rx_freq = (int64_t)strtod(optarg, NULL);
            break;
        case 'G':
            rx_gain = (int64_t)strtod(optarg, NULL);
            break;
        case 'a':
            agc_mode = parse_agc_mode(optarg);
            break;
        case 'T':
            tx_freq = (int64_t)strtod(optarg, NULL);
            break;
        case 'A':
            tx_gain = (int64_t)strtod(optarg, NULL);
            break;
        case 'C':
            num_channels = (uint8_t)atoi(optarg);
            if (num_channels != 1 && num_channels != 2) {
                fprintf(stderr, "Channels must be 1 or 2\n");
                return 1;
            }
            break;
        case 'r':
            rx_shm_path = optarg;
            break;
        case 't':
            tx_shm_path = optarg;
            break;
        case 'B':
            rx_buffer_time = strtod(optarg, NULL);
            break;
        case 'D':
            tx_buffer_time = strtod(optarg, NULL);
            break;
        case 'n':
            num_samples = (size_t)strtoull(optarg, NULL, 0);
            break;
        default:
            return 1;
        }
    }

    snprintf(g_device_path, sizeof(g_device_path), "/dev/m2sdr%d", g_device_num);

    printf("M2SDR Full-Duplex Stream\n");
    printf("========================\n");
    printf("Device: %s\n", g_device_path);
    printf("Sample rate: %.2f MHz\n", samplerate / 1e6);
    printf("Bandwidth: %.2f MHz\n", bandwidth / 1e6);
    printf("Channels: %d (%s)\n", num_channels, num_channels == 2 ? "2T2R" : "1T1R");
    printf("RX: freq=%.2f MHz, gain=%ld dB, AGC=%s\n",
           rx_freq / 1e6, rx_gain, agc_mode_str(agc_mode));
    printf("TX: freq=%.2f MHz, gain=%ld dB\n", tx_freq / 1e6, tx_gain);
    printf("RX SHM: %s (%.1fs buffer)\n", rx_shm_path, rx_buffer_time);
    printf("TX SHM: %s (%.1fs buffer)\n", tx_shm_path, tx_buffer_time);
    if (num_samples > 0)
        printf("Samples: %zu\n", num_samples);
    else
        printf("Samples: infinite (Ctrl+C to stop)\n");
    printf("\n");

    /* Open device for RF init */
    int fd = open(g_device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s\n", g_device_path);
        return 1;
    }

    /* Initialize RF (combined RX + TX) */
    rf_init(fd, samplerate, bandwidth, rx_freq, rx_gain, agc_mode,
            tx_freq, tx_gain, num_channels);
    close(fd);

    /* Create RX shared memory (this process is the RX producer) */
    shm_buffer_t *rx_shm = shm_create(rx_shm_path, DMA_BUFFER_SIZE, num_channels,
                                        rx_buffer_time, samplerate);
    if (!rx_shm) {
        fprintf(stderr, "Failed to create RX shared memory\n");
        return 1;
    }

    /* Open or create TX shared memory (this process is the TX consumer) */
    shm_buffer_t *tx_shm = NULL;

    if (wait_for_tx_shm) {
        printf("Waiting for TX shared memory at %s...\n", tx_shm_path);
        while (!tx_shm && g_keep_running) {
            tx_shm = shm_open_existing(tx_shm_path);
            if (!tx_shm)
                sleep(1);
        }
        if (!g_keep_running) {
            printf("Interrupted while waiting.\n");
            shm_destroy(rx_shm);
            return 1;
        }
    } else {
        tx_shm = shm_open_existing(tx_shm_path);
        if (!tx_shm) {
            printf("Creating new TX shared memory...\n");
            tx_shm = shm_create(tx_shm_path, DMA_BUFFER_SIZE, num_channels,
                                 tx_buffer_time, samplerate);
        }
    }

    if (!tx_shm) {
        fprintf(stderr, "Failed to open/create TX shared memory\n");
        shm_destroy(rx_shm);
        return 1;
    }

    /* Run full-duplex streaming */
    stream_duplex(rx_shm, tx_shm, num_samples, quiet);

    /* Cleanup */
    shm_destroy(rx_shm);
    shm_destroy(tx_shm);

    return 0;
}
