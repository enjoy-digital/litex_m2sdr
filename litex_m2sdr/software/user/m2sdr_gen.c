/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Generator Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
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
#include "m2sdr.h"
#include "config.h"
#include "csr.h"

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

static void m2sdr_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (m2sdr_reg_write(dev, addr, val) != 0) {
        fprintf(stderr, "CSR write failed @0x%08x\n", addr);
        exit(1);
    }
}

/* Signal (DMA TX) with GPIO PPS */
/*-----------------------------*/

static void m2sdr_gen(const char *device_id, double sample_rate, double frequency, double amplitude, uint8_t zero_copy, double pps_freq, uint8_t gpio_pin, const char *signal_type, uint8_t enable_header, uint8_t use_8bit) {
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;
    struct m2sdr_dev *dev = NULL;
    int fd = -1;

    /* Open device for CSR access */
    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }
    fd = m2sdr_get_fd(dev);
    if (fd < 0) {
        fprintf(stderr, "No PCIe fd available\n");
        m2sdr_close(dev);
        exit(1);
    }

    if (use_8bit) {
        m2sdr_write32(dev, CSR_AD9361_BITMODE_ADDR, 1);
    } else {
        m2sdr_write32(dev, CSR_AD9361_BITMODE_ADDR, 0);
    }

    if (enable_header) {
        if (m2sdr_set_tx_header(dev, true) != 0) {
            fprintf(stderr, "m2sdr_set_tx_header failed\n");
            m2sdr_close(dev);
            exit(1);
        }
    }

#ifdef CSR_GPIO_BASE

    /* Enable GPIO in Packer/Unpacker mode if PPS is requested */
    if (pps_freq > 0) {
        uint32_t control = (1 << CSR_GPIO_CONTROL_ENABLE_OFFSET) | (1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET); /* ENABLE=1, SOURCE=0 (DMA), LOOPBACK=1 */
        m2sdr_write32(dev, CSR_GPIO_CONTROL_ADDR, control);
        double pps_period_s = 1.0 / pps_freq;
        double pps_high_s = pps_period_s * 0.2;
        printf("GPIO Enabled for PPS/Toggle at %.2f Hz (20%% high: %.3fs, 80%% low: %.3fs) on bit %d\n",
               pps_freq, pps_high_s, pps_period_s - pps_high_s, gpio_pin);
    }

#endif

    /* Clamp amplitude to safe range */
    if (amplitude < 0.0)
        amplitude = 0.0;
    if (amplitude > 1.0)
        amplitude = 1.0;

    /* Print parameters */
    printf("Starting signal generation with parameters:\n");
    printf("  Device: %s\n", device_id);
    printf("  Sample Rate: %.0f Hz\n", sample_rate);
    printf("  Signal Type: %s\n", signal_type);
    if (strcmp(signal_type, "tone") == 0) {
        printf("  Frequency: %.0f Hz\n", frequency);
    }
    printf("  Amplitude: %.2f\n", amplitude);
    printf("  Zero-Copy Mode: %d\n", zero_copy);

    /* Initialize DMA */
    if (litepcie_dma_init(&dma, "", zero_copy)) {
        close(fd);
        m2sdr_close(dev);
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
    uint32_t lfsr = (uint32_t)rand() | 1u;

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
            int bytes_per_sample = use_8bit ? 4 : 8;
            int payload_bytes = DMA_BUFFER_SIZE - (enable_header ? 16 : 0);
            int num_samples = payload_bytes / bytes_per_sample;
            uint8_t *payload = (uint8_t *)buf_wr + (enable_header ? 16 : 0);

            if (enable_header) {
                uint64_t ts = 0;
                m2sdr_get_time(dev, &ts);
                ((uint64_t *)buf_wr)[0] = 0x5aa55aa55aa55aa5ULL;
                ((uint64_t *)buf_wr)[1] = ts;
            }
            for (int j = 0; j < num_samples; j++) {
                float I = 0.0;
                float Q = 0.0;

                if (strcmp(signal_type, "tone") == 0) {
                    I = cos(phi) * amplitude;
                    Q = sin(phi) * amplitude;
                    phi += omega;
                    if (phi >= 2 * M_PI) phi -= 2 * M_PI;
                } else if (strcmp(signal_type, "white") == 0) {
                    /* Fast LFSR-based noise (avoid rand() in hot loop) */
                    lfsr ^= lfsr << 13;
                    lfsr ^= lfsr >> 17;
                    lfsr ^= lfsr << 5;
                    int16_t ni = (int16_t)(lfsr & 0xFFFF);
                    int16_t nq = (int16_t)((lfsr >> 16) & 0xFFFF);
                    I = (ni / 32768.0f) * amplitude;
                    Q = (nq / 32768.0f) * amplitude;
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

                if (use_8bit) {
                    int8_t I_int = (int8_t)(I * 127);
                    int8_t Q_int = (int8_t)(Q * 127);
                    ((int8_t *)payload)[4 * j + 0] = I_int;
                    ((int8_t *)payload)[4 * j + 1] = Q_int;
                    ((int8_t *)payload)[4 * j + 2] = I_int;
                    ((int8_t *)payload)[4 * j + 3] = Q_int;
                } else {
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

                    ((int16_t *)payload)[4 * j + 0] = (I_int & 0xFFF) | (gpio1 << 12); // TX1_I
                    ((int16_t *)payload)[4 * j + 1] = (Q_int & 0xFFF) | (gpio1 << 12); // TX1_Q
                    ((int16_t *)payload)[4 * j + 2] = (I_int & 0xFFF) | (gpio2 << 12); // TX2_I
                    ((int16_t *)payload)[4 * j + 3] = (Q_int & 0xFFF) | (gpio2 << 12); // TX2_Q
                }

                sample_count++;
            }
        }

        /* Statistics every 200ms */
        int64_t duration = get_time_ms() - last_time;
        if (duration > 200) {
            /* Print banner every 10 lines */
            if (i % 10 == 0)
                printf("\e[1mSPEED(Gbps)   BUFFERS   SIZE(MB)   UNDERFLOWS\e[0m\n");
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
    m2sdr_write32(dev, CSR_GPIO_CONTROL_ADDR, 0);
#endif
    close(fd);
    m2sdr_close(dev);
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
           "-8                               Use 8-bit samples (SC8).\n"
           "-H                               Enable TX DMA header.\n"
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
    uint8_t enable_header = 0;
    uint8_t use_8bit = 0;
    char signal_type[16] = "tone"; /* Default to tone */

    signal(SIGINT, intHandler);

    /* Parameters */
    for (;;) {
        c = getopt(argc, argv, "hc:s:t:f:a:zp:g:8H");
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
        case '8':
            use_8bit = 1;
            break;
        case 'H':
            enable_header = 1;
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
    snprintf(m2sdr_device, sizeof(m2sdr_device), "pcie:/dev/m2sdr%d", m2sdr_device_num);

    /* Generate and play tone with optional PPS */
    m2sdr_gen(m2sdr_device, sample_rate, frequency, amplitude, m2sdr_device_zero_copy, pps_freq, gpio_pin, signal_type, enable_header, use_8bit);

    return 0;
}
