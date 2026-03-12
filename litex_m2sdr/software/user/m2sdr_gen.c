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
 *     ./m2sdr_rf -samplerate 30.72e6 -tx_freq 100e6 -tx-att 10
 *     ./m2sdr_gen -s 30.72e6 -t tone -f 1e6 -a 1.0
 *     ./m2sdr_gen -s 30.72e6 -t white -a 1.0
 *     ./m2sdr_gen -s 30.72e6 -t prbs -a 1.0
 *
 * TX control uses positive attenuation values across both the native and SoapySDR paths.
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
#include <getopt.h>

#include "liblitepcie.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "kissfft/kiss_fft.h"
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

struct ofdm_state {
    int fft_size;
    int cp_len;
    int occupied_carriers;
    int bits_per_symbol;
    uint32_t lfsr;
    kiss_fft_cfg ifft_cfg;
    kiss_fft_cpx *freq_bins;
    kiss_fft_cpx *time_bins;
    float *symbol_i;
    float *symbol_q;
    int symbol_len;
    int sample_index;
};

static void *xcalloc(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return ptr;
}

static uint32_t prng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0)
        x = 1;
    *state = x;
    return x;
}

static kiss_fft_cpx ofdm_modulate_symbol(struct ofdm_state *st)
{
    kiss_fft_cpx out = {0.0f, 0.0f};

    if (st->bits_per_symbol == 2) {
        out.r = (prng_next(&st->lfsr) & 1) ? 0.70710678f : -0.70710678f;
        out.i = (prng_next(&st->lfsr) & 1) ? 0.70710678f : -0.70710678f;
        return out;
    }

    if (st->bits_per_symbol == 4) {
        static const float levels[4] = {-3.0f, -1.0f, 3.0f, 1.0f};
        out.r = levels[prng_next(&st->lfsr) & 3] * 0.31622777f;
        out.i = levels[prng_next(&st->lfsr) & 3] * 0.31622777f;
        return out;
    }

    out.r = (prng_next(&st->lfsr) & 1) ? 1.0f : -1.0f;
    out.i = 0.0f;
    return out;
}

static void ofdm_generate_symbol(struct ofdm_state *st)
{
    float max_mag = 0.0f;
    int half = st->occupied_carriers / 2;

    memset(st->freq_bins, 0, sizeof(*st->freq_bins) * st->fft_size);
    memset(st->time_bins, 0, sizeof(*st->time_bins) * st->fft_size);

    for (int i = 0; i < half; i++) {
        st->freq_bins[1 + i] = ofdm_modulate_symbol(st);
        st->freq_bins[st->fft_size - half + i] = ofdm_modulate_symbol(st);
    }

    kiss_fft(st->ifft_cfg, st->freq_bins, st->time_bins);

    for (int i = 0; i < st->fft_size; i++) {
        st->time_bins[i].r /= st->fft_size;
        st->time_bins[i].i /= st->fft_size;
        float mag = hypotf(st->time_bins[i].r, st->time_bins[i].i);
        if (mag > max_mag)
            max_mag = mag;
    }

    if (max_mag < 1e-9f)
        max_mag = 1.0f;

    for (int i = 0; i < st->cp_len; i++) {
        int src = st->fft_size - st->cp_len + i;
        st->symbol_i[i] = st->time_bins[src].r / max_mag;
        st->symbol_q[i] = st->time_bins[src].i / max_mag;
    }
    for (int i = 0; i < st->fft_size; i++) {
        st->symbol_i[st->cp_len + i] = st->time_bins[i].r / max_mag;
        st->symbol_q[st->cp_len + i] = st->time_bins[i].i / max_mag;
    }

    st->sample_index = 0;
}

static void ofdm_init(struct ofdm_state *st, int fft_size, int cp_len, int occupied_carriers, const char *modulation, uint32_t seed)
{
    size_t cfg_size = 0;

    memset(st, 0, sizeof(*st));
    st->fft_size = fft_size;
    st->cp_len = cp_len;
    st->occupied_carriers = occupied_carriers;
    st->symbol_len = fft_size + cp_len;
    st->lfsr = seed ? seed : 1;

    if (strcmp(modulation, "qpsk") == 0)
        st->bits_per_symbol = 2;
    else if (strcmp(modulation, "16qam") == 0)
        st->bits_per_symbol = 4;
    else {
        fprintf(stderr, "Unsupported OFDM modulation: %s\n", modulation);
        exit(1);
    }

    kiss_fft_alloc(st->fft_size, 1, NULL, &cfg_size);
    st->ifft_cfg = kiss_fft_alloc(st->fft_size, 1, malloc(cfg_size), &cfg_size);
    if (!st->ifft_cfg) {
        fprintf(stderr, "Failed to allocate OFDM IFFT state\n");
        exit(1);
    }

    st->freq_bins = xcalloc(st->fft_size, sizeof(*st->freq_bins));
    st->time_bins = xcalloc(st->fft_size, sizeof(*st->time_bins));
    st->symbol_i = xcalloc(st->symbol_len, sizeof(*st->symbol_i));
    st->symbol_q = xcalloc(st->symbol_len, sizeof(*st->symbol_q));
    ofdm_generate_symbol(st);
}

static void ofdm_cleanup(struct ofdm_state *st)
{
    if (!st)
        return;
    kiss_fft_free(st->ifft_cfg);
    free(st->freq_bins);
    free(st->time_bins);
    free(st->symbol_i);
    free(st->symbol_q);
    memset(st, 0, sizeof(*st));
}

static void ofdm_next_sample(struct ofdm_state *st, float *i_out, float *q_out)
{
    if (st->sample_index >= st->symbol_len)
        ofdm_generate_symbol(st);
    *i_out = st->symbol_i[st->sample_index];
    *q_out = st->symbol_q[st->sample_index];
    st->sample_index++;
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

static void m2sdr_gen(const char *device_id, double sample_rate, double frequency, double amplitude, uint8_t zero_copy, double pps_freq, uint8_t gpio_pin, const char *signal_type, uint8_t enable_header, uint8_t use_8bit, int ofdm_fft_size, int ofdm_cp_len, int ofdm_occupied_carriers, const char *ofdm_modulation, uint32_t ofdm_seed) {
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;
    struct m2sdr_dev *dev = NULL;
    int fd = -1;
    struct ofdm_state ofdm;
    int ofdm_enabled = strcmp(signal_type, "ofdm") == 0;

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
    } else if (ofdm_enabled) {
        printf("  OFDM FFT Size: %d\n", ofdm_fft_size);
        printf("  OFDM CP Length: %d\n", ofdm_cp_len);
        printf("  OFDM Occupied Carriers: %d\n", ofdm_occupied_carriers);
        printf("  OFDM Modulation: %s\n", ofdm_modulation);
        printf("  OFDM Seed: %" PRIu32 "\n", ofdm_seed);
    }
    printf("  Amplitude: %.2f\n", amplitude);
    printf("  Zero-Copy Mode: %d\n", zero_copy);

    /* Initialize DMA */
    dma.shared_fd = 1;
    dma.fds.fd = fd;
    if (litepcie_dma_init(&dma, "", zero_copy)) {
        m2sdr_close(dev);
        exit(1);
    }

    dma.reader_enable = 1;

    if (ofdm_enabled)
        ofdm_init(&ofdm, ofdm_fft_size, ofdm_cp_len, ofdm_occupied_carriers, ofdm_modulation, ofdm_seed);

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
                } else if (strcmp(signal_type, "ofdm") == 0) {
                    ofdm_next_sample(&ofdm, &I, &Q);
                    I *= amplitude;
                    Q *= amplitude;
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
    if (ofdm_enabled)
        ofdm_cleanup(&ofdm);
#ifdef CSR_GPIO_BASE
    m2sdr_write32(dev, CSR_GPIO_CONTROL_ADDR, 0);
#endif
    m2sdr_close(dev);
}

/* Help */
/*------*/

static void help(void) {
    printf("M2SDR Signal Generator Utility\n"
           "usage: m2sdr_gen [options]\n"
           "\n"
           "Options:\n"
           "  -h, --help                     Show this help message.\n"
           "  -d, --device DEV               Use explicit device id.\n"
           "  -c, --device-num N             Select PCIe device number (default: 0).\n"
           "      --sample-rate HZ           Set sample rate in Hz (default: 30720000).\n"
           "      --signal TYPE              Set signal type: tone, white, prbs, or ofdm.\n"
           "      --tone-freq HZ             Set tone frequency in Hz (default: 1000).\n"
           "      --ofdm-fft-size N          OFDM FFT size (default: 1024).\n"
           "      --ofdm-cp-len N            OFDM cyclic prefix length (default: 32).\n"
           "      --ofdm-carriers N          OFDM occupied carriers (default: 200).\n"
           "      --ofdm-modulation MOD      OFDM modulation: qpsk or 16qam (default: qpsk).\n"
           "      --ofdm-seed N              OFDM PRBS seed (default: 1).\n"
           "      --amplitude A              Set amplitude (0.0 to 1.0, default: 1.0).\n"
           "      --pps-freq HZ              Enable PPS/toggle on GPIO.\n"
           "      --gpio-pin N               Select GPIO pin for PPS (0-3, default: 0).\n"
           "      --enable-header            Enable TX DMA header.\n"
           "      --format FMT               Sample format: sc16 or sc8 (default: sc16).\n"
           "      --zero-copy                Request zero-copy DMA mode.\n"
           "      --8bit                     Legacy alias for --format sc8.\n"
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv) {
    int c;
    int option_index = 0;
    static uint8_t m2sdr_device_zero_copy = 0;
    double sample_rate = 30720000.0;
    double frequency = 1000.0;
    double amplitude = 1.0;
    double pps_freq = 0.0; /* Disabled by default */
    uint8_t gpio_pin = 0;  /* Default to GPIO bit 0 */
    uint8_t enable_header = 0;
    uint8_t use_8bit = 0;
    int ofdm_fft_size = 1024;
    int ofdm_cp_len = 32;
    int ofdm_occupied_carriers = 200;
    uint32_t ofdm_seed = 1;
    char signal_type[16] = "tone"; /* Default to tone */
    char ofdm_modulation[16] = "qpsk";
    struct m2sdr_cli_device cli_dev;
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "sample-rate", required_argument, NULL, 's' },
        { "signal", required_argument, NULL, 't' },
        { "tone-freq", required_argument, NULL, 'f' },
        { "amplitude", required_argument, NULL, 'a' },
        { "zero-copy", no_argument, NULL, 'z' },
        { "pps-freq", required_argument, NULL, 'p' },
        { "gpio-pin", required_argument, NULL, 'g' },
        { "enable-header", no_argument, NULL, 'H' },
        { "ofdm-fft-size", required_argument, NULL, 3 },
        { "ofdm-cp-len", required_argument, NULL, 4 },
        { "ofdm-carriers", required_argument, NULL, 5 },
        { "ofdm-modulation", required_argument, NULL, 6 },
        { "ofdm-seed", required_argument, NULL, 7 },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);

    /* Parameters */
    for (;;) {
        c = getopt_long(argc, argv, "hd:c:s:t:f:a:zp:g:H", options, &option_index);
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd':
        case 'c':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                exit(1);
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
            if (strcmp(signal_type, "tone") != 0 && strcmp(signal_type, "white") != 0 && strcmp(signal_type, "prbs") != 0 && strcmp(signal_type, "ofdm") != 0) {
                m2sdr_cli_invalid_choice("signal", signal_type, "tone, white, prbs, or ofdm");
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
        case 1:
            if (!strcmp(optarg, "sc16")) {
                use_8bit = 0;
            } else if (!strcmp(optarg, "sc8")) {
                use_8bit = 1;
            } else {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case 2:
            use_8bit = 1;
            break;
        case 'H':
            enable_header = 1;
            break;
        case 3:
            ofdm_fft_size = atoi(optarg);
            break;
        case 4:
            ofdm_cp_len = atoi(optarg);
            break;
        case 5:
            ofdm_occupied_carriers = atoi(optarg);
            break;
        case 6:
            strncpy(ofdm_modulation, optarg, sizeof(ofdm_modulation) - 1);
            ofdm_modulation[sizeof(ofdm_modulation) - 1] = '\0';
            if (strcmp(ofdm_modulation, "qpsk") != 0 && strcmp(ofdm_modulation, "16qam") != 0) {
                m2sdr_cli_invalid_choice("ofdm modulation", ofdm_modulation, "qpsk or 16qam");
                return 1;
            }
            break;
        case 7:
            ofdm_seed = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        default:
            exit(1);
        }
    }

    if (ofdm_fft_size <= 0 || ofdm_cp_len < 0 || ofdm_cp_len >= ofdm_fft_size) {
        fprintf(stderr, "Invalid OFDM parameters: require fft-size > 0 and 0 <= cp-len < fft-size\n");
        return 1;
    }
    if (ofdm_occupied_carriers <= 0 || ofdm_occupied_carriers >= ofdm_fft_size || (ofdm_occupied_carriers & 1)) {
        fprintf(stderr, "Invalid OFDM occupied carriers: require an even value in [2, fft-size-1]\n");
        return 1;
    }

    /* Show help only if no options are provided */
    if (argc == 1) {
        help();
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        return 1;

    /* Generate and play tone with optional PPS */
    m2sdr_gen(m2sdr_cli_device_id(&cli_dev), sample_rate, frequency, amplitude, m2sdr_device_zero_copy, pps_freq, gpio_pin, signal_type, enable_header, use_8bit, ofdm_fft_size, ofdm_cp_len, ofdm_occupied_carriers, ofdm_modulation, ofdm_seed);

    return 0;
}
