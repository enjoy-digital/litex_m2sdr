/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Generator Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * This utility generates tone, white noise, or PRBS signals into interleaved 16-bit I/Q samples for
 * use with the M2SDR software-defined radio. It runs in real-time using DMA to transmit the generated signal.
 *
 * Note: Configure the RF settings first using m2sdr_rf before running this utility.
 *
 * Usage Example:
 *     ./m2sdr_rf -samplerate 30.72e6 -tx_freq 100e6 -tx_gain -10
 *     ./m2sdr_gen -s 30.72e6 -t tone -f 1e6 -a 1.0
 *     ./m2sdr_gen -s 30.72e6 -t white -a 1.0
 *     ./m2sdr_gen -s 30.72e6 -t prbs -a 1.0
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <time.h>

#include "liblitepcie.h"

/* Variables */
/*-----------*/

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* PRBS bit generator (PRBS31: x^31 + x^28 + 1) */
static uint32_t prbs_lfsr = 0xFFFFFFFFu; /* Initial seed, all 1s */

static int get_prbs_bit(void) {
    int bit = ((prbs_lfsr >> 30) ^ (prbs_lfsr >> 27)) & 1;
    prbs_lfsr = (prbs_lfsr << 1) | bit;
    return bit;
}

/* Signal (DMA TX) with GPIO PPS */
/*-----------------------------*/

static void m2sdr_gen(const char *device_name, double sample_rate, double frequency, double amplitude, uint8_t zero_copy, double pps_freq, uint8_t gpio_pin, const char *signal_type) {
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;
    int fd;

    /* Open device for CSR access */
    fd = open(device_name, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        exit(1);
    }

#ifdef CSR_GPIO_BASE

    /* Enable GPIO in Packer/Unpacker mode if PPS is requested */
    if (pps_freq > 0) {
        uint32_t control = (1 << CSR_GPIO_CONTROL_ENABLE_OFFSET) | (1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET); /* ENABLE=1, SOURCE=0 (DMA), LOOPBACK=1 */
        litepcie_writel(fd, CSR_GPIO_CONTROL_ADDR, control);
        double pps_period_s = 1.0 / pps_freq;
        double pps_high_s = pps_period_s * 0.2;
        printf("GPIO Enabled for PPS/Toggle at %.2f Hz (20%% high: %.3fs, 80%% low: %.3fs) on bit %d\n",
               pps_freq, pps_high_s, pps_period_s - pps_high_s, gpio_pin);
    }

#endif

    /* Print parameters */
    printf("Starting signal generation with parameters:\n");
    printf("  Device: %s\n", device_name);
    printf("  Sample Rate: %.0f Hz\n", sample_rate);
    printf("  Signal Type: %s\n", signal_type);
    if (strcmp(signal_type, "tone") == 0) {
        printf("  Frequency: %.0f Hz\n", frequency);
    }
    printf("  Amplitude: %.2f\n", amplitude);
    printf("  Zero-Copy Mode: %d\n", zero_copy);

    /* Initialize DMA */
    if (litepcie_dma_init(&dma, device_name, zero_copy)) {
        close(fd);
        exit(1);
    }

    dma.reader_enable = 1;

    /* Tone generation parameters */
    double phi = 0.0;
    double omega = 2 * M_PI * frequency / sample_rate;

    /* PPS/Toggle parameters */
    double pps_period_samples = pps_freq > 0 ? sample_rate / pps_freq : 0;
    double pps_high_samples = pps_period_samples * 0.2; /* 20% high */
    uint64_t sample_count = 0;

    /* Seed random number generator for white noise */
    srand(time(NULL));

    /* Test Loop */
    last_time = get_time_ms();
    for (;;) {
        /* Update DMA status */
        litepcie_dma_process(&dma);

        /* Exit loop on CTRL+C */
        if (!keep_running) {
            hw_count_stop = dma.reader_sw_count + 16;
            break;
        }

        /* Write to DMA */
        while (1) {
            /* Get Write buffer */
            char *buf_wr = litepcie_dma_next_write_buffer(&dma);
            /* Break when no buffer available for Write */
            if (!buf_wr) {
                break;
            }
            /* Detect DMA underflows */
            if (dma.reader_sw_count - dma.reader_hw_count < 0)
                sw_underflows += (dma.reader_hw_count - dma.reader_sw_count);

            /* Generate tone and fill Write buffer */
            int num_samples = DMA_BUFFER_SIZE / 8; // 8 bytes per sample: TX1_I, TX1_Q, TX2_I, TX2_Q
            *(uint64_t *)buf_wr = 0x5aa55aa55aa55aa5ULL;
            ((int16_t *)buf_wr)[4 * 1 + 0] = (sample_count & 0xFFF);
            ((int16_t *)buf_wr)[4 * 1 + 1] = (sample_count & 0xFFF);
            ((int16_t *)buf_wr)[4 * 1 + 2] = (sample_count & 0xFFF);
            ((int16_t *)buf_wr)[4 * 1 + 3] = (sample_count & 0xFFF);

            for (int j = 2; j < num_samples; j++) {
                float I = 0.0;
                float Q = 0.0;

                if (strcmp(signal_type, "tone") == 0) {
                    I = cos(phi) * amplitude;
                    Q = sin(phi) * amplitude;
                    phi += omega;
                    if (phi >= 2 * M_PI) phi -= 2 * M_PI;
                } else if (strcmp(signal_type, "white") == 0) {
                    I = ((float)rand() / RAND_MAX * 2.0 - 1.0) * amplitude;
                    Q = ((float)rand() / RAND_MAX * 2.0 - 1.0) * amplitude;
                } else if (strcmp(signal_type, "prbs") == 0) {
                    int32_t value_I = 0;
                    int32_t value_Q = 0;
                    for (int b = 0; b < 12; b++) {
                        value_I = (value_I << 1) | get_prbs_bit();
                    }
                    for (int b = 0; b < 12; b++) {
                        value_Q = (value_Q << 1) | get_prbs_bit();
                    }
                    I = ((float)(value_I - 2048) / 2048.0f) * amplitude;
                    Q = ((float)(value_Q - 2048) / 2048.0f) * amplitude;
                }

                int16_t I_int = (int16_t)(I * 2047); // Scale to 12-bit range
                int16_t Q_int = (int16_t)(Q * 2047); // Scale to 12-bit range

                /* GPIO PPS/Toggle on selected bit */
                uint16_t gpio1 = 0, gpio2 = 0;
                if (pps_freq > 0) {
                    double phase = fmod(sample_count, pps_period_samples);
                    if (phase < pps_high_samples) {
                        gpio1 = 1 << gpio_pin; // Selected bit high (first edge)
                        gpio2 = 1 << gpio_pin; // Selected bit high (second edge)
                    }
                }

                ((int16_t *)buf_wr)[4 * j + 0] = (I_int & 0xFFF) | (gpio1 << 12); // TX1_I (IA[15:12] = GPIO1)
                ((int16_t *)buf_wr)[4 * j + 1] = (Q_int & 0xFFF) | (gpio1 << 12); // TX1_Q (QA[15:12] = GPIO1 OE)
                ((int16_t *)buf_wr)[4 * j + 2] = (I_int & 0xFFF) | (gpio2 << 12); // TX2_I (IB[15:12] = GPIO2)
                ((int16_t *)buf_wr)[4 * j + 3] = (Q_int & 0xFFF) | (gpio2 << 12); // TX2_Q (QB[15:12] = GPIO2_OE)

                sample_count++;
            }
        }

        /* Statistics every 200ms */
        int64_t duration = get_time_ms() - last_time;
        if (duration > 200) {
            /* Print banner every 10 lines */
            if (i % 10 == 0) {
                printf("\e[1mSPEED(Gbps)   BUFFERS   SIZE(MB)   UNDERFLOWS\e[0m\n");
                uint64_t last_timestamp = litepcie_readl(fd, CSR_HEADER_LAST_TX_TIMESTAMP_ADDR);
                printf("[FROM CSR] Last TX Timestamp = %llu\n", last_timestamp);
                uint64_t last_header = litepcie_readl(fd, CSR_HEADER_LAST_TX_HEADER_ADDR);
                printf("[FROM CSR] Last TX Header = %llu\n", last_header);
                // uint64_t scheduler_status = litepcie_readl(fd, CSR_AD9361_SCHEDULER_TX_STATUS_ADDR);
                // uint8_t sink_ready, sink_valid, source_ready, source_valid;
                // sink_valid = scheduler_status & 0b1;
                // sink_ready = scheduler_status & 0b10;
                // source_valid = scheduler_status & 0b100;
                // source_ready = scheduler_status & 0b1000;

                // printf("[FROM CSR] sink_valid = %u, sink_ready = %u, source_valid = %u, source_ready = %u\n", sink_valid,sink_ready,
                //                                                                                             source_valid,source_ready);
            
                printf("writing timestamp %lu\n", sample_count);
            }
                
            i++;
            /* Print statistics */
            printf("%10.2f %10" PRIu64 " %10" PRIu64 " %10ld\n",
                   (double)(dma.reader_sw_count - reader_sw_count_last) * DMA_BUFFER_SIZE * 8 / ((double)duration * 1e6),
                   dma.reader_sw_count,
                   (dma.reader_sw_count * DMA_BUFFER_SIZE) / 1024 / 1024,
                   sw_underflows);
            /* Update time/count/underflows */
            last_time = get_time_ms();
            reader_sw_count_last = dma.reader_sw_count;
            sw_underflows = 0;
        }
    }

    /* Wait end of DMA transfer */
    while (dma.reader_hw_count < hw_count_stop) {
        dma.reader_enable = 1;
        litepcie_dma_process(&dma);
    }

    /* Cleanup DMA and close device */
    litepcie_dma_cleanup(&dma);
#ifdef CSR_GPIO_BASE
    litepcie_writel(fd, CSR_GPIO_CONTROL_ADDR, 0);
#endif
    close(fd);
}

/* Help */
/*------*/

static void help(void) {
    printf("M2SDR Signal Generator Utility\n"
           "usage: m2sdr_gen [options]\n"
           "\n"
           "Options:\n"
           "-h                               Help.\n"
           "-c device_num                    Select the device (default = 0).\n"
           "-s sample_rate                   Set sample rate in Hz (default = 30720000).\n"
           "-t signal_type                   Set signal type: 'tone' (default), 'white', or 'prbs'.\n"
           "-f frequency                     Set tone frequency in Hz (default = 1000).\n"
           "-a amplitude                     Set amplitude (0.0 to 1.0, default = 1.0).\n"
           "-z                               Enable zero-copy DMA mode.\n"
           "-p [pps_freq]                    Enable PPS/toggle on GPIO (default 1 Hz, 20%% high).\n"
           "-g gpio_pin                      Select GPIO pin for PPS (0-3, default 0).\n"
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv) {
    int c;
    static char m2sdr_device[1024];
    static int m2sdr_device_num = 0;
    static uint8_t m2sdr_device_zero_copy = 0;
    double sample_rate = 30720000.0;
    double frequency = 1000.0;
    double amplitude = 1.0;
    double pps_freq = 0.0; /* Disabled by default */
    uint8_t gpio_pin = 0;  /* Default to GPIO bit 0 */
    char signal_type[16] = "tone"; /* Default to tone */

    signal(SIGINT, intHandler);

    /* Parameters */
    for (;;) {
        c = getopt(argc, argv, "hc:s:t:f:a:zp:g:");
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            break;
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
        case 's':
            sample_rate = atof(optarg);
            if (sample_rate <= 0) {
                fprintf(stderr, "Sample rate must be positive\n");
                exit(1);
            }
            break;
        case 't':
            strncpy(signal_type, optarg, sizeof(signal_type) - 1);
            signal_type[sizeof(signal_type) - 1] = '\0';
            if (strcmp(signal_type, "tone") != 0 && strcmp(signal_type, "white") != 0 && strcmp(signal_type, "prbs") != 0) {
                fprintf(stderr, "Invalid signal type: must be 'tone', 'white', or 'prbs'\n");
                exit(1);
            }
            break;
        case 'f':
            frequency = atof(optarg);
            if (frequency < 0) {
                fprintf(stderr, "Frequency must be non-negative\n");
                exit(1);
            }
            break;
        case 'a':
            amplitude = atof(optarg);
            if (amplitude < 0.0 || amplitude > 1.0) {
                fprintf(stderr, "Amplitude must be between 0.0 and 1.0\n");
                exit(1);
            }
            break;
        case 'z':
            m2sdr_device_zero_copy = 1;
            break;
        case 'p':
            if (optarg) {
                pps_freq = atof(optarg); /* Use provided argument */
            } else if (optind < argc && argv[optind][0] != '-') {
                pps_freq = atof(argv[optind++]); /* Use next argument if no space */
            } else {
                pps_freq = 1.0; /* Default to 1 Hz */
            }
            if (pps_freq <= 0) {
                fprintf(stderr, "PPS/toggle frequency must be positive\n");
                exit(1);
            }
            break;
        case 'g':
            gpio_pin = atoi(optarg);
            if (gpio_pin > 3) {
                fprintf(stderr, "GPIO pin must be between 0 and 3\n");
                exit(1);
            }
            break;
        default:
            exit(1);
        }
    }

    /* Show help only if no options are provided */
    if (argc == 1) {
        help();
    }

    /* Select device */
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);

    /* Generate and play tone with optional PPS */
    m2sdr_gen(m2sdr_device, sample_rate, frequency, amplitude, m2sdr_device_zero_copy, pps_freq, gpio_pin, signal_type);

    return 0;
}
