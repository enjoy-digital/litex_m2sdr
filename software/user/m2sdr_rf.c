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
static int litepcie_execute_and_exit;

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

//#define BIST_TX_TONE
//#define BIST_RX_TONE
//#define BIST_PRBS_TEST

static void m2sdr_init(
    uint32_t samplerate,
    int64_t  refclk_freq,
    int64_t  tx_freq,
    int64_t  rx_freq,
    int64_t  tx_gain,
    int64_t  rx_gain,
    uint8_t  loopback,
    uint8_t  execute_and_exit
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
    ad9361_init(&ad9361_phy, &default_init_param);

    /* Configure AD9361 TX/RX FIRs */
    ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    /* Configure AD9361 Samplerate */
    ad9361_set_trx_clock_chain_freq(ad9361_phy, samplerate);

    /* Configure AD9361 TX Attenuation */
    ad9361_set_tx_atten(ad9361_phy, -tx_gain*1000, 1, 1, 1);

    /* Configure AD9361 RX Gain */
    ad9361_set_rx_rf_gain(ad9361_phy, 0, rx_gain);
    ad9361_set_rx_rf_gain(ad9361_phy, 1, rx_gain);

    /* Configure AD9361 RX->TX Loopback */
    ad9361_bist_loopback(ad9361_phy, loopback);

    /* Debug/Tests */
    printf("SPI Register 0x010—Parallel Port Configuration 1: %08x\n", m2sdr_ad9361_spi_read(fd, 0x10));
    printf("SPI Register 0x011—Parallel Port Configuration 2: %08x\n", m2sdr_ad9361_spi_read(fd, 0x11));
    printf("SPI Register 0x012—Parallel Port Configuration 3: %08x\n", m2sdr_ad9361_spi_read(fd, 0x12));

    printf("AD9361 Control: %08x\n", litepcie_readl(fd, CSR_AD9361_CONFIG_ADDR));

    litepcie_writel(fd, CSR_AD9361_PRBS_TX_ADDR, 0 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));

#ifdef BIST_TX_TONE
    printf("BIST_TX_TONE_TEST...\n");
    ad9361_bist_tone(ad9361_phy, BIST_INJ_TX, 1000000, 0, 0x0); /* 1MHz tone / 0dB / RX1&2 */
#endif

#ifdef BIST_RX_TONE
    printf("BIST_RX_TONE_TEST...\n");
    ad9361_bist_tone(ad9361_phy, BIST_INJ_RX, 1000000, 0, 0x0); /* 1MHz tone / 0dB / RX1&2 */
#endif

#ifdef BIST_PRBS_TEST
    int rx_clk_delay;
    int rx_dat_delay;
    int tx_clk_delay;
    int tx_dat_delay;
    printf("BIST_PRBS_TEST...\n");

    /* Enable AD9361 RX-PRBS */
    ad9361_bist_prbs(ad9361_phy, BIST_INJ_RX);

    /* RX Clk/Dat delays scan */
    printf("\n");
    printf("RX Clk/Dat delays scan...\n");
    printf("-------------------------\n");

    /* Loop on RX Clk delay */
    printf("Clk/Dat |  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15\n");
    for (rx_clk_delay=0; rx_clk_delay<16; rx_clk_delay++){
        /* Loop on RX Dat delay */
        printf(" %2d     |", rx_clk_delay);
        for (rx_dat_delay=0; rx_dat_delay<16; rx_dat_delay++) {
            /* Configure Clk/Dat delays */
            m2sdr_ad9361_spi_write(fd, REG_RX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(rx_clk_delay) | RX_DATA_DELAY(rx_dat_delay));

            /* Small sleep to let PRBS synchronize */
            mdelay(10);

            /* Check PRBS checker synchronization */
            printf(" %2d", litepcie_readl(fd, CSR_AD9361_PRBS_RX_ADDR) & 0x1);
        }
        printf("\n");
    }

    /* Configure RX Clk/Dat delays */
    m2sdr_ad9361_spi_write(fd, REG_RX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(RX_CLK_DELAY) | RX_DATA_DELAY(RX_DAT_DELAY));

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
    for (tx_clk_delay=0; tx_clk_delay<16; tx_clk_delay++){
        /* Loop on TX Dat delay */
        printf(" %2d     |", tx_clk_delay);
        for (tx_dat_delay=0; tx_dat_delay<16; tx_dat_delay++) {
            /* Configure Clk/Dat delays */
            m2sdr_ad9361_spi_write(fd, REG_TX_CLOCK_DATA_DELAY, DATA_CLK_DELAY(tx_clk_delay) | RX_DATA_DELAY(tx_dat_delay));

            /* Small sleep to let PRBS synchronize */
            mdelay(10);

            /* Check PRBS checker synchronization */
            printf(" %2d", litepcie_readl(fd, CSR_AD9361_PRBS_RX_ADDR) & 0x1);
        }
        printf("\n");
    }

    /* Disable FPGA TX-PRBS */
    litepcie_writel(fd, CSR_AD9361_PRBS_TX_ADDR, 0 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));

    /* Enable RX->TX AD9361 loopback */
    ad9361_bist_loopback(ad9361_phy, 0);

    /* Disable AD9361 RX-PRBS */
    ad9361_bist_prbs(ad9361_phy, 0);

#endif

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
           "-x                    Execute and exit.\n"
           "\n"
           "-refclk_freq freq     Set the RefClk frequency in Hz (default=%" PRId64 ").\n"
           "-samplerate sps       Set RF Samplerate in SPS (default=%d).\n"
           "-tx_freq freq         Set the TX (TX1/2) frequency in Hz (default=%" PRId64 ").\n"
           "-rx_freq freq         Set the RX (RX1/2) frequency in Hz (default=%" PRId64 ").\n"
           "-tx_gain gain         Set the TX gain in dB (default=%d).\n"
           "-rx_gain gain         Set the RX gain in dB (default=%d).\n"
           "-loopback enable      Set the internal loopback (JESD Deframer -> Framer) (default=%d).\n"
           "\n",
           DEFAULT_REFCLK_FREQ,
           DEFAULT_SAMPLERATE,
           DEFAULT_TX_FREQ,
           DEFAULT_RX_FREQ,
           DEFAULT_TX_GAIN,
           DEFAULT_RX_GAIN,
           DEFAULT_LOOPBACK);
    exit(1);
}

static struct option options[] = {
    { "help",             no_argument, NULL, 'h' },   /*  0 */
    { "execute_and_exit", no_argument, NULL, 'x' },   /*  1 */
    { "refclk_freq",      required_argument },        /*  2 */
    { "samplerate",       required_argument },        /*  3 */
    { "tx_freq",          required_argument },        /*  4 */
    { "rx_freq",          required_argument },        /*  5 */
    { "tx_gain",          required_argument },        /*  6 */
    { "rx_gain",          required_argument },        /*  7 */
    { "loopback",         required_argument },        /*  8 */
    { NULL },
};

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;
    int option_index;

    litepcie_device_num       = 0;
    litepcie_execute_and_exit = 0;

    int64_t  refclk_freq;
    uint32_t samplerate;
    int64_t  tx_freq, rx_freq;
    int64_t  tx_gain, rx_gain;
    uint8_t  loopback;

    refclk_freq  = DEFAULT_REFCLK_FREQ;
    samplerate   = DEFAULT_SAMPLERATE;
    tx_freq      = DEFAULT_TX_FREQ;
    rx_freq      = DEFAULT_RX_FREQ;
    tx_gain      = DEFAULT_TX_GAIN;
    rx_gain      = DEFAULT_RX_GAIN;
    loopback     = DEFAULT_LOOPBACK;

    /* Parse/Handle Parameters. */
    for (;;) {
        c = getopt_long_only(argc, argv, "hx", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0 :
            switch(option_index) {
                case 2: /* refclk_freq */
                    refclk_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 3: /* samplerate */
                    samplerate = (uint32_t)strtod(optarg, NULL);
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
        case 'x':
            litepcie_execute_and_exit = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/litepcie%d", litepcie_device_num);

    /* Initialize RF. */
    m2sdr_init(samplerate, refclk_freq, tx_freq, rx_freq, tx_gain, rx_gain, loopback, litepcie_execute_and_exit);

    return 0;
}
