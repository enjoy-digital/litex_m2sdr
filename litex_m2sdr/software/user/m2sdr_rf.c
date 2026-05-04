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
#include <stdbool.h>
#include <getopt.h>
#include <stdint.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_config.h"

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR RF Utility\n"
           "usage: m2sdr_rf [options]\n"
           "\n"
           "Options:\n"
           "  -h, --help             Show this help message and exit.\n"
           "  -d, --device DEV       Use explicit device id.\n"
#ifdef USE_LITEPCIE
           "  -c, --device-num N     Select PCIe device number (default: 0).\n"
#elif defined(USE_LITEETH)
           "  -i, --ip ADDR          Target IP address for Etherbone.\n"
           "  -p, --port PORT        Port number (default: 1234).\n"
#endif
           "      --format FMT       Sample format: sc16 or sc8 (default: sc16).\n"
           "      --8bit             Legacy alias for --format sc8.\n"
           "      --oversample       Enable oversample mode.\n"
           "      --channel-layout M Channel mode: 1t1r or 2t2r (default: 2t2r).\n"
           "      --sync MODE        Clock source: internal, external, or fpga.\n"
           "\n"
           "      --refclk-freq HZ   Set the RefClk frequency in Hz (default: %" PRId64 ").\n"
           "      --sample-rate SPS  Set RF sample rate in SPS (default: %d, accepts 30.72e6 or 20M).\n"
           "      --bandwidth HZ     Set the RF bandwidth in Hz (default: %d).\n"
           "      --tx-freq HZ       Set the TX frequency in Hz (default: %" PRId64 ").\n"
           "      --rx-freq HZ       Set the RX frequency in Hz (default: %" PRId64 ").\n"
           "      --tx-att DB        Set TX attenuation in dB (default: %d).\n"
           "      --rx-gain DB       Set both RX gains in dB and force manual gain mode.\n"
           "      --rx-gain1 DB      Set RX gain 1 in dB and force manual gain mode.\n"
           "      --rx-gain2 DB      Set RX gain 2 in dB and force manual gain mode.\n"
           "      --loopback N       Set internal loopback (default: %d).\n"
           "      --bist-tx-tone     Run TX tone test.\n"
           "      --bist-rx-tone     Run RX tone test.\n"
           "      --bist-prbs        Run PRBS test.\n"
           "      --calibrate-delay  Scan and program FPGA <-> AD9361 RX/TX clock/data delays using PRBS.\n"
           "      --bist-tone-freq HZ Set the BIST tone frequency in Hz (default: %d).\n",
           DEFAULT_REFCLK_FREQ,
           DEFAULT_SAMPLERATE,
           DEFAULT_BANDWIDTH,
           DEFAULT_TX_FREQ,
           DEFAULT_RX_FREQ,
           DEFAULT_TX_ATT,
           DEFAULT_LOOPBACK,
           DEFAULT_BIST_TONE_FREQ);
    exit(1);
}

/* Main */
/*------*/

static int parse_i64_option(const char *name, const char *value, int64_t *out)
{
    if (m2sdr_cli_parse_int64(value, out) == 0)
        return 0;

    m2sdr_cli_error("invalid %s '%s' (expected integer, scientific notation, or K/M/G suffix)",
                    name ? name : "value",
                    value ? value : "(null)");
    return -1;
}

int main(int argc, char **argv)
{
    int c;
    struct m2sdr_config cfg;
    struct m2sdr_cli_device cli_dev;
    int option_index = 0;
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { "oversample", no_argument, NULL, 3 },
        { "channel-layout", required_argument, NULL, 4 },
        { "chan", required_argument, NULL, 4 },
        { "sync", required_argument, NULL, 5 },
        { "refclk-freq", required_argument, NULL, 6 },
        { "refclk_freq", required_argument, NULL, 6 },
        { "sample-rate", required_argument, NULL, 7 },
        { "samplerate", required_argument, NULL, 7 },
        { "bandwidth", required_argument, NULL, 8 },
        { "tx-freq", required_argument, NULL, 9 },
        { "tx_freq", required_argument, NULL, 9 },
        { "rx-freq", required_argument, NULL, 10 },
        { "rx_freq", required_argument, NULL, 10 },
        { "tx-att", required_argument, NULL, 11 },
        { "tx_att", required_argument, NULL, 11 },
        { "rx-gain", required_argument, NULL, 12 },
        { "rx_gain", required_argument, NULL, 12 },
        { "rx-gain1", required_argument, NULL, 13 },
        { "rx_gain1", required_argument, NULL, 13 },
        { "rx-gain2", required_argument, NULL, 14 },
        { "rx_gain2", required_argument, NULL, 14 },
        { "loopback", required_argument, NULL, 15 },
        { "bist-tx-tone", no_argument, NULL, 16 },
        { "bist_tx_tone", no_argument, NULL, 16 },
        { "bist-rx-tone", no_argument, NULL, 17 },
        { "bist_rx_tone", no_argument, NULL, 17 },
        { "bist-prbs", no_argument, NULL, 18 },
        { "bist_prbs", no_argument, NULL, 18 },
        { "calibrate-delay", no_argument, NULL, 20 },
        { "calibrate_delay", no_argument, NULL, 20 },
        { "dig-tune-prbs", no_argument, NULL, 20 },
        { "dig_tune_prbs", no_argument, NULL, 20 },
        { "bist-tone-freq", required_argument, NULL, 19 },
        { "bist_tone_freq", required_argument, NULL, 19 },
        { NULL, 0, NULL, 0 }
    };
    m2sdr_config_init(&cfg);

    m2sdr_cli_device_init(&cli_dev);

    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:p:", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            return 0;
        case 'd':
        case 'c':
        case 'i':
        case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                exit(1);
            break;
        case 1:
            if (!strcmp(optarg, "sc16"))
                cfg.enable_8bit_mode = false;
            else if (!strcmp(optarg, "sc8"))
                cfg.enable_8bit_mode = true;
            else {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case 2:
            cfg.enable_8bit_mode = true;
            break;
        case 3:
            cfg.enable_oversample = true;
            break;
        case 4:
            cfg.chan_mode = optarg;
            if (strcmp(optarg, "1t1r") == 0)
                cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_1T1R;
            else if (strcmp(optarg, "2t2r") == 0)
                cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
            else {
                m2sdr_cli_invalid_choice("channel layout", optarg, "1t1r or 2t2r");
                return 1;
            }
            break;
        case 5:
            cfg.sync_mode = optarg;
            if (strcmp(optarg, "internal") == 0)
                cfg.clock_source = M2SDR_CLOCK_SOURCE_INTERNAL;
            else if (strcmp(optarg, "external") == 0)
                cfg.clock_source = M2SDR_CLOCK_SOURCE_EXTERNAL;
            else if ((strcmp(optarg, "fpga") == 0) ||
                     (strcmp(optarg, "si5351c-fpga") == 0) ||
                     (strcmp(optarg, "si5351c_fpga") == 0) ||
                     (strcmp(optarg, "pll") == 0))
                cfg.clock_source = M2SDR_CLOCK_SOURCE_SI5351C_FPGA;
            else {
                m2sdr_cli_invalid_choice("sync mode", optarg, "internal, external, or fpga");
                return 1;
            }
            break;
        case 6:
            if (parse_i64_option("refclk frequency", optarg, &cfg.refclk_freq) != 0)
                return 1;
            break;
        case 7:
            if (parse_i64_option("sample rate", optarg, &cfg.sample_rate) != 0)
                return 1;
            break;
        case 8:
            if (parse_i64_option("bandwidth", optarg, &cfg.bandwidth) != 0)
                return 1;
            break;
        case 9:
            if (parse_i64_option("TX frequency", optarg, &cfg.tx_freq) != 0)
                return 1;
            break;
        case 10:
            if (parse_i64_option("RX frequency", optarg, &cfg.rx_freq) != 0)
                return 1;
            break;
        case 11:
            if (parse_i64_option("TX attenuation", optarg, &cfg.tx_att) != 0)
                return 1;
            break;
        case 12:
            if (parse_i64_option("RX gain", optarg, &cfg.rx_gain1) != 0)
                return 1;
            cfg.rx_gain2 = cfg.rx_gain1;
            cfg.program_rx_gains = true;
            break;
        case 13:
            if (parse_i64_option("RX gain 1", optarg, &cfg.rx_gain1) != 0)
                return 1;
            cfg.program_rx_gains = true;
            break;
        case 14:
            if (parse_i64_option("RX gain 2", optarg, &cfg.rx_gain2) != 0)
                return 1;
            cfg.program_rx_gains = true;
            break;
        case 15:
            cfg.loopback = (uint8_t)strtoul(optarg, NULL, 0);
            break;
        case 16:
            cfg.bist_tx_tone = true;
            break;
        case 17:
            cfg.bist_rx_tone = true;
            break;
        case 18:
            cfg.bist_prbs = true;
            break;
        case 19:
            cfg.bist_tone_freq = (int32_t)strtoul(optarg, NULL, 0);
            break;
        case 20:
            cfg.calibrate_interface_delay = true;
            break;
        default:
            exit(1);
        }
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        exit(1);

    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
        fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
        return 1;
    }

    {
        int rc = m2sdr_apply_config(dev, &cfg);
        if (rc != 0) {
            fprintf(stderr, "m2sdr_apply_config failed: %s\n", m2sdr_strerror(rc));
            m2sdr_close(dev);
            return 1;
        }
    }

    m2sdr_close(dev);
    return 0;
}
