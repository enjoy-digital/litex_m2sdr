/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR RF Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
 *
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

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "m2sdr_config.h"

#include "liblitepcie.h"
#include "libm2sdr.h"

/* Variables */
/*-----------*/

static char litepcie_device[1024];
static int litepcie_device_num;

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* AD9361 */
/*--------*/

#define AD9361_GPIO_RESET_PIN 0

struct ad9361_rf_phy *ad9361_phy;

int spi_write_then_read(struct spi_device *spi,
                        const unsigned char *txbuf, unsigned n_tx,
                        unsigned char *rxbuf, unsigned n_rx)
{

    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    if (n_tx == 2 && n_rx == 1) {
        /* read */
        rxbuf[0] = m2sdr_ad9361_spi_read(fd, txbuf[0] << 8 | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        /* write */
        m2sdr_ad9361_spi_write(fd, txbuf[0] << 8 | txbuf[1], txbuf[2]);
    } else {
        fprintf(stderr, "Unsupported SPI transfer n_tx=%d n_rx=%d\n",
                n_tx, n_rx);
        exit(1);
    }

    close(fd);

    return 0;
}

void udelay(unsigned long usecs)
{
    usleep(usecs);
}

void mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

bool gpio_is_valid(int number)
{
 switch(number) {
    case AD9361_GPIO_RESET_PIN:
        return true;
    default:
        return false;
    }
}

void gpio_set_value(unsigned gpio, int value)
{

}

/* M2SDR Init */
/*------------*/

static void m2sdr_init(
    uint32_t samplerate,
    int64_t  bandwidth,
    int64_t  refclk_freq,
    int64_t  tx_freq,
    int64_t  rx_freq,
    int64_t  tx_gain,
    int64_t  rx_gain,
    uint8_t  loopback,
    bool     bist_tx_tone,
    bool     bist_rx_tone,
    bool     bist_prbs,
    int32_t  bist_tone_freq
) {
    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    /* Initialize SI531 Clocking */
    m2sdr_si5351_i2c_config(fd, SI5351_I2C_ADDR, si5351_config, sizeof(si5351_config)/sizeof(si5351_config[0]));

    /* Initialize AD9361 SPI */
    m2sdr_ad9361_spi_init(fd);

    /* Initialize AD9361 RFIC */
    default_init_param.gpio_resetb  = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync    = -1;
    default_init_param.gpio_cal_sw1 = -1;
    default_init_param.gpio_cal_sw2 = -1;
    ad9361_init(&ad9361_phy, &default_init_param, 1);

    /* Configure AD9361 Samplerate */
    ad9361_set_tx_sampling_freq(ad9361_phy, samplerate);
    ad9361_set_rx_sampling_freq(ad9361_phy, samplerate);

    /* Configure AD9361 TX/RX Bandwidth */
    ad9361_set_rx_rf_bandwidth(ad9361_phy, bandwidth);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, bandwidth);

    /* Configure AD9361 TX/RX Frequencies */
    ad9361_set_tx_lo_freq(ad9361_phy, tx_freq);
    ad9361_set_rx_lo_freq(ad9361_phy, rx_freq);

    /* Configure AD9361 TX/RX FIRs */
    ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    /* Configure AD9361 TX Attenuation */
    ad9361_set_tx_atten(ad9361_phy, -tx_gain*1000, 1, 1, 1);

    /* Configure AD9361 RX Gain */
    ad9361_set_rx_rf_gain(ad9361_phy, 0, rx_gain);
    ad9361_set_rx_rf_gain(ad9361_phy, 1, rx_gain);

    /* Configure AD9361 RX->TX Loopback */
    ad9361_bist_loopback(ad9361_phy, loopback);

    /* Configure 2T2R/1T1R mode */
#ifdef _1T1R_MODE
    litepcie_writel(fd, CSR_AD9361_PHY_CONTROL_ADDR, 1);
#else
    litepcie_writel(fd, CSR_AD9361_PHY_CONTROL_ADDR, 0);
#endif

    /* Enable BIST TX Tone (Optional: For RF TX Tests) */
    if (bist_tx_tone) {
        printf("BIST_TX_TONE_TEST...\n");
        ad9361_bist_tone(ad9361_phy, BIST_INJ_TX, bist_tone_freq, 0, 0x0); /* tone / 0dB / RX1&2 */
    }

    /* Enable BIST RX Tone (Optional: For Software RX Tests) */
    if (bist_rx_tone) {
        printf("BIST_RX_TONE_TEST...\n");
        ad9361_bist_tone(ad9361_phy, BIST_INJ_RX, bist_tone_freq, 0, 0x0); /* tone / 0dB / RX1&2 */
    }

    /* Enable BIST PRBS Test (Optional: For FPGA <-> AD9361 interface calibration) */
    if (bist_prbs) {
        int rx_clk_delay, rx_dat_delay, tx_clk_delay, tx_dat_delay;
        int rx_valid_delays[16][16] = {{0}};
        int tx_valid_delays[16][16] = {{0}};

        printf("BIST_PRBS TEST...\n");

        /* Enable AD9361 RX-PRBS */
        litepcie_writel(fd, CSR_AD9361_PRBS_TX_ADDR, 0 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));
        ad9361_bist_prbs(ad9361_phy, BIST_INJ_RX);

        /* RX Clk/Dat delays scan */
        printf("\n");
        printf("RX Clk/Dat delays scan...\n");
        printf("-------------------------\n");

        /* Loop on RX Clk delay */
        printf("Clk/Dat |  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15\n");
        for (rx_clk_delay = 0; rx_clk_delay < 16; rx_clk_delay++) {
            /* Loop on RX Dat delay */
            printf(" %2d     |", rx_clk_delay);
            for (rx_dat_delay = 0; rx_dat_delay < 16; rx_dat_delay++) {
                /* Configure Clk/Dat delays */
                m2sdr_ad9361_spi_write(fd, REG_RX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(rx_clk_delay) | RX_DATA_DELAY(rx_dat_delay));

                /* Small sleep to let PRBS synchronize */
                mdelay(10);

                /* Check PRBS checker synchronization */
                int prbs_sync = litepcie_readl(fd, CSR_AD9361_PRBS_RX_ADDR) & 0x1;
                printf(" %2d", prbs_sync);

                /* Record valid delay settings */
                rx_valid_delays[rx_clk_delay][rx_dat_delay] = prbs_sync;
            }
            printf("\n");
        }

        /* Find optimal RX Clk/Dat delays */
        int optimal_rx_clk_delay = -1, optimal_rx_dat_delay = -1;
        for (rx_clk_delay = 0; rx_clk_delay < 16; rx_clk_delay++) {
            for (rx_dat_delay = 0; rx_dat_delay < 16; rx_dat_delay++) {
                if (rx_valid_delays[rx_clk_delay][rx_dat_delay] == 1) {
                    optimal_rx_clk_delay = rx_clk_delay;
                    optimal_rx_dat_delay = rx_dat_delay;
                    break;
                }
            }
            if (optimal_rx_clk_delay != -1 && optimal_rx_dat_delay != -1) {
                break;
            }
        }

        /* Display optimal RX Clk/Dat delays */
        if (optimal_rx_clk_delay != -1 && optimal_rx_dat_delay != -1) {
            printf("Optimal RX Clk Delay: %d, Optimal RX Dat Delay: %d\n", optimal_rx_clk_delay, optimal_rx_dat_delay);
        }

        /* Configure optimal RX Clk/Dat delays */
        if (optimal_rx_clk_delay != -1 && optimal_rx_dat_delay != -1) {
            m2sdr_ad9361_spi_write(fd, REG_RX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(optimal_rx_clk_delay) | RX_DATA_DELAY(optimal_rx_dat_delay));
        }

        /* Enable RX->TX AD9361 loopback */
        ad9361_bist_loopback(ad9361_phy, 1);

        /* Enable FPGA TX-PRBS */
        litepcie_writel(fd, CSR_AD9361_PRBS_TX_ADDR, 1 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));

        /* TX Clk/Dat delays scan */
        printf("\n");
        printf("TX Clk/Dat delays scan...\n");
        printf("-------------------------\n");

        /* Loop on TX Clk delay */
        printf("Clk/Dat |  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15\n");
        for (tx_clk_delay = 0; tx_clk_delay < 16; tx_clk_delay++) {
            /* Loop on TX Dat delay */
            printf(" %2d     |", tx_clk_delay);
            for (tx_dat_delay = 0; tx_dat_delay < 16; tx_dat_delay++) {
                /* Configure Clk/Dat delays */
                m2sdr_ad9361_spi_write(fd, REG_TX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(tx_clk_delay) | RX_DATA_DELAY(tx_dat_delay));

                /* Small sleep to let PRBS synchronize */
                mdelay(10);

                /* Check PRBS checker synchronization */
                int prbs_sync = litepcie_readl(fd, CSR_AD9361_PRBS_RX_ADDR) & 0x1;
                printf(" %2d", prbs_sync);

                /* Record valid delay settings */
                tx_valid_delays[tx_clk_delay][tx_dat_delay] = prbs_sync;
            }
            printf("\n");
        }

        /* Find optimal TX Clk/Dat delays */
        int optimal_tx_clk_delay = -1, optimal_tx_dat_delay = -1;
        for (tx_clk_delay = 0; tx_clk_delay < 16; tx_clk_delay++) {
            for (tx_dat_delay = 0; tx_dat_delay < 16; tx_dat_delay++) {
                if (tx_valid_delays[tx_clk_delay][tx_dat_delay] == 1) {
                    optimal_tx_clk_delay = tx_clk_delay;
                    optimal_tx_dat_delay = tx_dat_delay;
                    break;
                }
            }
            if (optimal_tx_clk_delay != -1 && optimal_tx_dat_delay != -1) {
                break;
            }
        }

        /* Configure optimal TX Clk/Dat delays */
        if (optimal_tx_clk_delay != -1 && optimal_tx_dat_delay != -1) {
            m2sdr_ad9361_spi_write(fd, REG_TX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(optimal_tx_clk_delay) | RX_DATA_DELAY(optimal_tx_dat_delay));
        }

        /* Display optimal TX Clk/Dat delays */
        if (optimal_tx_clk_delay != -1 && optimal_tx_dat_delay != -1) {
            printf("Optimal TX Clk Delay: %d, Optimal TX Dat Delay: %d\n", optimal_tx_clk_delay, optimal_tx_dat_delay);
        }

        /* Display calibration results */
        printf("\n");
        printf("Calibration completed:\n");
        printf("----------------------\n");
        printf("RX Clk:%d/Dat:%d, TX Clk:%d/Dat:%d\n",
               optimal_rx_clk_delay,
               optimal_rx_dat_delay,
               optimal_tx_clk_delay,
               optimal_tx_dat_delay
        );
    }

    close(fd);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR RF init/config utility\n"
           "usage: m2sdr_rf [options] cmd [args...]\n"
           "\n"
           "Options:\n"
           "-h                    Help.\n"
           "-c device_num         Select the device (default=0).\n"
           "\n"
           "-refclk_freq freq     Set the RefClk frequency in Hz (default=%" PRId64 ").\n"
           "-samplerate sps       Set RF Samplerate in SPS (default=%d).\n"
           "-bandwidth bw         Set the RF bandwidth in Hz (default=%d).\n"
           "-tx_freq freq         Set the TX (TX1/2) frequency in Hz (default=%" PRId64 ").\n"
           "-rx_freq freq         Set the RX (RX1/2) frequency in Hz (default=%" PRId64 ").\n"
           "-tx_gain gain         Set the TX gain in dB (default=%d).\n"
           "-rx_gain gain         Set the RX gain in dB (default=%d).\n"
           "-loopback enable      Set the internal loopback (JESD Deframer -> Framer) (default=%d).\n"
           "-bist_tx_tone         Run TX tone test.\n"
           "-bist_rx_tone         Run RX tone test.\n"
           "-bist_prbs            Run PRBS test.\n"
           "-bist_tone_freq freq  Set the BIST tone frequency in Hz (default=%d).\n"
           "\n",
           DEFAULT_REFCLK_FREQ,
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_TX_FREQ,
           DEFAULT_RX_FREQ,
           DEFAULT_TX_GAIN,
           DEFAULT_RX_GAIN,
           DEFAULT_BIST_TONE_FREQ,
           DEFAULT_LOOPBACK);
    exit(1);
}

static struct option options[] = {
    { "help",             no_argument, NULL, 'h' },   /*  0 */
    { "refclk_freq",      required_argument },        /*  1 */
    { "samplerate",       required_argument },        /*  2 */
    { "bandwidth",        required_argument },        /*  3 */
    { "tx_freq",          required_argument },        /*  4 */
    { "rx_freq",          required_argument },        /*  5 */
    { "tx_gain",          required_argument },        /*  6 */
    { "rx_gain",          required_argument },        /*  7 */
    { "loopback",         required_argument },        /*  8 */
    { "bist_tx_tone",     no_argument },              /*  9 */
    { "bist_rx_tone",     no_argument },              /* 10 */
    { "bist_prbs",        no_argument },              /* 11 */
    { "bist_tone_freq",   required_argument },        /* 12 */
    { NULL },
};

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;
    int option_index;

    litepcie_device_num = 0;

    int64_t  refclk_freq;
    uint32_t samplerate;
    int32_t  bandwidth;
    int64_t  tx_freq, rx_freq;
    int64_t  tx_gain, rx_gain;
    uint8_t  loopback;
    bool     bist_tx_tone = false;
    bool     bist_rx_tone = false;
    bool     bist_prbs    = false;
    int32_t  bist_tone_freq;

    refclk_freq    = DEFAULT_REFCLK_FREQ;
    samplerate     = DEFAULT_SAMPLERATE;
    bandwidth      = DEFAULT_BANDWIDTH;
    tx_freq        = DEFAULT_TX_FREQ;
    rx_freq        = DEFAULT_RX_FREQ;
    tx_gain        = DEFAULT_TX_GAIN;
    rx_gain        = DEFAULT_RX_GAIN;
    loopback       = DEFAULT_LOOPBACK;
    bist_tone_freq = DEFAULT_BIST_TONE_FREQ;

    /* Parse/Handle Parameters. */
    for (;;) {
        c = getopt_long_only(argc, argv, "hx", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0 :
            switch(option_index) {
                case 1: /* refclk_freq */
                    refclk_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 2: /* samplerate */
                    samplerate = (uint32_t)strtod(optarg, NULL);
                    break;
                case 3: /* bandwidth */
                    bandwidth = (int32_t)strtod(optarg, NULL);
                    break;
                case 4: /* tx_freq */
                    tx_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 5: /* rx_freq */
                    rx_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 6: /* tx_gain */
                    tx_gain = (int64_t)strtod(optarg, NULL);
                    break;
                case 7: /* rx_gain */
                    rx_gain = (int64_t)strtod(optarg, NULL);
                    break;
                case 8: /* loopback */
                    loopback = (uint8_t)strtod(optarg, NULL);
                    break;
                case 9: /* bist_tx_tone */
                    bist_tx_tone = true;
                    break;
                case 10: /* bist_rx_tone */
                    bist_rx_tone = true;
                    break;
                case 11: /* bist_prbs */
                    bist_prbs = true;
                    break;
                case 12: /* bist_tone_freq */
                    bist_tone_freq = (int32_t)strtod(optarg, NULL);
                    break;
                default:
                    fprintf(stderr, "unknown option index: %d\n", option_index);
                    exit(1);
            }
            break;
        case 'h':
            help();
            exit(1);
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/litepcie%d", litepcie_device_num);

    /* Initialize RF. */
    m2sdr_init(samplerate, bandwidth, refclk_freq, tx_freq, rx_freq, tx_gain, rx_gain, loopback, bist_tx_tone, bist_rx_tone, bist_prbs, bist_tone_freq);

    return 0;
}
