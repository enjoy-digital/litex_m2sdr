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
           "  -c, --device-num N     Select PCIe device number (default: 0).\n"
           "  -i, --ip ADDR          Target IP address for Etherbone.\n"
           "  -p, --port PORT        Port number (default: 1234).\n"
           "      --format FMT       Sample format: sc16, sc8 or bfp8 (default: sc16).\n"
           "      --8bit             Legacy alias for --format sc8.\n"
           "      --channel-layout M Channel mode: 1t1r or 2t2r (default: 2t2r).\n"
           "      --sync MODE        Clock source: internal, external, or fpga.\n"
           "\n"
           "      --refclk-freq HZ   Set the RefClk frequency in Hz (default: %" PRId64 ").\n"
           "      --refclk-ppm PPM   Compensate a measured RefClk error in ppm through the\n"
           "                         SI5351 PLL (positive = clock runs fast, default: 0).\n"
           "      --sample-rate SPS  Set RF sample rate in SPS (default: %d, accepts 30.72e6\n"
           "                         or 20M). Rates above 61.44 MSPS select the\n"
           "                         wide-bandwidth mode (~100 MHz analog passband).\n"
           "      --bandwidth HZ     Set the RF bandwidth in Hz (default: %d).\n"
           "      --tx-freq HZ       Set the TX frequency in Hz (default: %" PRId64 ").\n"
           "      --rx-freq HZ       Set the RX frequency in Hz (default: %" PRId64 ").\n"
           "      --tx-att DB        Set TX attenuation in dB (default: %d).\n"
           "      --rx-gain-mode M   Set both RX gain modes: manual, slow, fast, or hybrid.\n"
           "      --rx-gain-mode1 M  Set RX1 gain mode.\n"
           "      --rx-gain-mode2 M  Set RX2 gain mode.\n"
           "      --rx-gain DB       Set both RX gains in dB and force manual gain mode.\n"
           "      --rx-gain1 DB      Set RX gain 1 in dB and force manual gain mode.\n"
           "      --rx-gain2 DB      Set RX gain 2 in dB and force manual gain mode.\n"
           "      --rx-agc-pin BOOL  Drive the FPGA-connected AD9361 EN_AGC pin.\n"
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
        { "channel-layout", required_argument, NULL, 4 },
        { "chan", required_argument, NULL, 4 },
        { "sync", required_argument, NULL, 5 },
        { "refclk-freq", required_argument, NULL, 6 },
        { "refclk_freq", required_argument, NULL, 6 },
        { "refclk-ppm", required_argument, NULL, 25 },
        { "refclk_ppm", required_argument, NULL, 25 },
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
        { "rx-gain-mode", required_argument, NULL, 21 },
        { "rx_gain_mode", required_argument, NULL, 21 },
        { "rx-gain-mode1", required_argument, NULL, 22 },
        { "rx_gain_mode1", required_argument, NULL, 22 },
        { "rx-gain-mode2", required_argument, NULL, 23 },
        { "rx_gain_mode2", required_argument, NULL, 23 },
        { "rx-agc-pin", required_argument, NULL, 24 },
        { "rx_agc_pin", required_argument, NULL, 24 },
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
            {
                enum m2sdr_format format;

                if (m2sdr_cli_parse_format(optarg, &format) != 0) {
                    m2sdr_cli_invalid_choice("format", optarg, "sc16, sc8 or bfp8");
                    return 1;
                }
                cfg.sample_format = format;
                cfg.enable_8bit_mode = (format == M2SDR_FORMAT_SC8_Q7);
            }
            break;
        case 2:
            cfg.enable_8bit_mode = true;
            cfg.sample_format = M2SDR_FORMAT_SC8_Q7;
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
        case 25:
            if (m2sdr_cli_parse_double_range(optarg, -M2SDR_REFCLK_PPM_MAX,
                                             M2SDR_REFCLK_PPM_MAX, &cfg.refclk_ppm) != 0) {
                m2sdr_cli_error("invalid refclk ppm '%s' (expected -%.0f to %.0f)",
                                optarg, M2SDR_REFCLK_PPM_MAX, M2SDR_REFCLK_PPM_MAX);
                return 1;
            }
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
        case 21:
            if (m2sdr_parse_rx_gain_mode(optarg, &cfg.rx_gain_mode1) != M2SDR_ERR_OK) {
                m2sdr_cli_invalid_choice("RX gain mode", optarg, "manual, slow, fast, or hybrid");
                return 1;
            }
            cfg.rx_gain_mode2 = cfg.rx_gain_mode1;
            cfg.program_rx_gain_modes = true;
            break;
        case 22:
            if (m2sdr_parse_rx_gain_mode(optarg, &cfg.rx_gain_mode1) != M2SDR_ERR_OK) {
                m2sdr_cli_invalid_choice("RX gain mode 1", optarg, "manual, slow, fast, or hybrid");
                return 1;
            }
            cfg.program_rx_gain_modes = true;
            break;
        case 23:
            if (m2sdr_parse_rx_gain_mode(optarg, &cfg.rx_gain_mode2) != M2SDR_ERR_OK) {
                m2sdr_cli_invalid_choice("RX gain mode 2", optarg, "manual, slow, fast, or hybrid");
                return 1;
            }
            cfg.program_rx_gain_modes = true;
            break;
        case 24:
            if (m2sdr_cli_parse_bool(optarg, &cfg.agc_pin_enable) != 0) {
                m2sdr_cli_invalid_choice("RX AGC pin", optarg, "0/1, true/false, on/off, or yes/no");
                return 1;
            }
            cfg.program_agc_pin = true;
            break;
        case 15:
            {
                unsigned loopback;

                if (m2sdr_cli_parse_uint_range(optarg, 0, 255, &loopback) != 0) {
                    m2sdr_cli_error("invalid loopback value '%s'", optarg);
                    return 1;
                }
                cfg.loopback = (uint8_t)loopback;
            }
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
            {
                int tone_freq;

                if (m2sdr_cli_parse_int_range(optarg, 0, INT32_MAX, &tone_freq) != 0) {
                    m2sdr_cli_error("invalid BIST tone frequency '%s'", optarg);
                    return 1;
                }
                cfg.bist_tone_freq = (int32_t)tone_freq;
            }
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
