/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR RF Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdint.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_config.h"

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR RF Utility\n"
           "usage: m2sdr_rf [options]\n"
           "\n"
           "Options:\n"
           "  -h                     Show this help message and exit.\n"
#ifdef USE_LITEPCIE
           "  -c device_num          Select the device (default: 0).\n"
#elif defined(USE_LITEETH)
           "  -i ip_address          Target IP address for Etherbone (required).\n"
           "  -p port                Port number (default = 1234).\n"
#endif
           "  -8bit                  Enable 8-bit mode (default: disabled).\n"
           "  -oversample            Enable oversample mode (default: disabled).\n"
           "  -chan mode             Set channel mode: '1t1r' or '2t2r' (default: '2t2r').\n"
           "  -sync mode             Set synchronization mode ('internal' or 'external', default: internal).\n"
           "\n"
           "  -refclk_freq freq      Set the RefClk frequency in Hz (default: %" PRId64 ").\n"
           "  -samplerate sps        Set RF samplerate in SPS (default: %d).\n"
           "  -bandwidth bw          Set the RF bandwidth in Hz (default: %d).\n"
           "  -tx_freq freq          Set the TX frequency in Hz (default: %" PRId64 ").\n"
           "  -rx_freq freq          Set the RX frequency in Hz (default: %" PRId64 ").\n"
           "  -tx_gain gain          Set the TX gain in dB (default: %d).\n"
           "  -rx_gain gain          Set the RX gain in dB for both channels (default: %d).\n"
           "  -rx_gain1 gain         Set the RX gain in dB for channel 1 (default: %d).\n"
           "  -rx_gain2 gain         Set the RX gain in dB for channel 2 (default: %d).\n"
           "  -loopback enable       Set the internal loopback (default: %d).\n"
           "  -bist_tx_tone          Run TX tone test.\n"
           "  -bist_rx_tone          Run RX tone test.\n"
           "  -bist_prbs             Run PRBS test.\n"
           "  -bist_tone_freq freq   Set the BIST tone frequency in Hz (default: %d).\n",
           DEFAULT_REFCLK_FREQ,
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_TX_FREQ,
           DEFAULT_RX_FREQ,
           DEFAULT_TX_GAIN,
           DEFAULT_RX_GAIN,
           DEFAULT_RX_GAIN,
           DEFAULT_RX_GAIN,
           DEFAULT_LOOPBACK,
           DEFAULT_BIST_TONE_FREQ);
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;
    struct m2sdr_config cfg;
    struct m2sdr_cli_device cli_dev;
    m2sdr_config_init(&cfg);

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);

    for (;;) {
#if defined(USE_LITEPCIE)
        c = getopt(argc, argv, "hc:8");
#elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:8");
#endif
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
        case 'i':
        case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                exit(1);
            break;
        case '8':
            cfg.enable_8bit_mode = true;
            break;
        default:
            exit(1);
        }
    }

    while (optind < argc) {
        if (strcmp(argv[optind], "-8bit") == 0) {
            cfg.enable_8bit_mode = true;
        } else if (strcmp(argv[optind], "-oversample") == 0) {
            cfg.enable_oversample = true;
        } else if (strcmp(argv[optind], "-chan") == 0 && optind + 1 < argc) {
            const char *mode = argv[++optind];
            cfg.chan_mode = mode;
            if (strcmp(mode, "1t1r") == 0)
                cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_1T1R;
            else if (strcmp(mode, "2t2r") == 0)
                cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
            else
                help();
        } else if (strcmp(argv[optind], "-sync") == 0 && optind + 1 < argc) {
            const char *mode = argv[++optind];
            cfg.sync_mode = mode;
            if (strcmp(mode, "internal") == 0)
                cfg.clock_source = M2SDR_CLOCK_SOURCE_INTERNAL;
            else if (strcmp(mode, "external") == 0)
                cfg.clock_source = M2SDR_CLOCK_SOURCE_EXTERNAL;
            else
                help();
        } else if (strcmp(argv[optind], "-refclk_freq") == 0 && optind + 1 < argc) {
            cfg.refclk_freq = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-samplerate") == 0 && optind + 1 < argc) {
            cfg.sample_rate = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-bandwidth") == 0 && optind + 1 < argc) {
            cfg.bandwidth = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-tx_freq") == 0 && optind + 1 < argc) {
            cfg.tx_freq = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-rx_freq") == 0 && optind + 1 < argc) {
            cfg.rx_freq = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-tx_gain") == 0 && optind + 1 < argc) {
            cfg.tx_gain = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-rx_gain") == 0 && optind + 1 < argc) {
            cfg.rx_gain1 = strtoll(argv[++optind], NULL, 0);
            cfg.rx_gain2 = cfg.rx_gain1;
        } else if (strcmp(argv[optind], "-rx_gain1") == 0 && optind + 1 < argc) {
            cfg.rx_gain1 = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-rx_gain2") == 0 && optind + 1 < argc) {
            cfg.rx_gain2 = strtoll(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-loopback") == 0 && optind + 1 < argc) {
            cfg.loopback = (uint8_t)strtoul(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "-bist_tx_tone") == 0) {
            cfg.bist_tx_tone = true;
        } else if (strcmp(argv[optind], "-bist_rx_tone") == 0) {
            cfg.bist_rx_tone = true;
        } else if (strcmp(argv[optind], "-bist_prbs") == 0) {
            cfg.bist_prbs = true;
        } else if (strcmp(argv[optind], "-bist_tone_freq") == 0 && optind + 1 < argc) {
            cfg.bist_tone_freq = (int32_t)strtoul(argv[++optind], NULL, 0);
        } else {
            help();
        }
        optind++;
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        exit(1);

    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
        fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
        return 1;
    }

    if (m2sdr_apply_config(dev, &cfg) != 0) {
        fprintf(stderr, "m2sdr_apply_config failed\n");
        m2sdr_close(dev);
        return 1;
    }

    m2sdr_close(dev);
    return 0;
}
