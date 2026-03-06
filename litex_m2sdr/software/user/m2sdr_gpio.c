/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR GPIO Utility.
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
#include <fcntl.h>

#include "libm2sdr.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"

/* Variables */
/*-----------*/

/* Connection Functions */
/*----------------------*/

static struct m2sdr_dev *g_dev = NULL;
static struct m2sdr_cli_device g_cli_dev;

static struct m2sdr_dev *m2sdr_open_dev(void) {
    if (g_dev)
        return g_dev;
    if (!m2sdr_cli_finalize_device(&g_cli_dev)) {
        exit(1);
    }
    if (m2sdr_open(&g_dev, m2sdr_cli_device_id(&g_cli_dev)) != 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }
    return g_dev;
}

static void m2sdr_close_dev(struct m2sdr_dev *dev) {
    (void)dev;
    if (g_dev) {
        m2sdr_close(g_dev);
        g_dev = NULL;
    }
}


/* GPIO Control Functions */
/*-----------------------*/

static void configure_gpio(struct m2sdr_dev *dev, uint8_t gpio_enable, uint8_t loopback_enable, uint8_t source_csr, uint32_t output_data, uint32_t output_enable) {
    if (gpio_enable && source_csr) {
        if (output_data & ~0xF) {
            fprintf(stderr, "GPIO output_data out of range (4-bit): 0x%x\n", output_data);
            return;
        }
        if (output_enable & ~0xF) {
            fprintf(stderr, "GPIO output_enable out of range (4-bit): 0x%x\n", output_enable);
            return;
        }
    }

    if (m2sdr_gpio_config(dev, gpio_enable ? true : false, loopback_enable ? true : false, source_csr ? true : false) != 0) {
        fprintf(stderr, "GPIO config failed\n");
        return;
    }

    if (gpio_enable && source_csr) {
        if (m2sdr_gpio_write(dev, output_data & 0xF, output_enable & 0xF) != 0) {
            fprintf(stderr, "GPIO write failed\n");
            return;
        }
    }

    uint8_t input_data = 0;
    if (m2sdr_gpio_read(dev, &input_data) != 0) {
        fprintf(stderr, "GPIO read failed\n");
        return;
    }

    printf("GPIO Control: %s, Source: %s, Loopback: %s\n",
           gpio_enable ? "Enabled" : "Disabled",
           gpio_enable && source_csr ? "CSR" : "DMA",
           gpio_enable && loopback_enable ? "Enabled" : "Disabled");
    printf("GPIO Output Data: 0x%01x, Output Enable: 0x%01x, Input Data: 0x%01x\n",
           gpio_enable && source_csr ? (output_data & 0xF) : 0,
           gpio_enable && source_csr ? (output_enable & 0xF) : 0,
           input_data & 0xF);
}

/* Help */
/*------*/

static void help(void) {
    printf("M2SDR GPIO Utility\n"
           "usage: m2sdr_gpio [options]\n"
           "\n"
           "Options:\n"
           "-h                    Display this help message.\n"
#ifdef USE_LITEPCIE
           "-c device_num         Select the device (default = 0).\n"
#elif defined(USE_LITEETH)
           "-i ip_address         Target IP address of the board (required).\n"
           "-p port               Port number (default = 1234).\n"
#endif
           "-g                    Enable GPIO control.\n"
           "-l                    Enable GPIO loopback mode (requires -g).\n"
           "-s                    Use CSR mode instead of DMA mode (requires -g).\n"
           "-o output_data        Set GPIO output data (4-bit hex, e.g., 0xF, requires -s).\n"
           "-e output_enable      Set GPIO output enable (4-bit hex, e.g., 0xF, requires -s).\n");
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv) {
    int c;
    static uint8_t gpio_enable = 0;
    static uint8_t loopback_enable = 0;
    static uint8_t source_csr = 0;
    static uint32_t output_data = 0;
    static uint32_t output_enable = 0;

    m2sdr_cli_device_init(&g_cli_dev);
    /* Parameters */
    for (;;) {
        #ifdef USE_LITEPCIE
        c = getopt(argc, argv, "hc:glso:e:");
        #elif defined(USE_LITEETH)
        c = getopt(argc, argv, "hi:p:glso:e:");
        #endif
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            break;
        case 'c':
        case 'i':
        case 'p':
            if (m2sdr_cli_handle_device_option(&g_cli_dev, c, optarg) != 0)
                exit(1);
            break;
        case 'g':
            gpio_enable = 1;
            break;
        case 'l':
            loopback_enable = 1;
            break;
        case 's':
            source_csr = 1;
            break;
        case 'o':
            output_data = strtoul(optarg, NULL, 0);
            break;
        case 'e':
            output_enable = strtoul(optarg, NULL, 0);
            break;
        default:
            exit(1);
        }
    }

    /* Validate options */
    if (loopback_enable && !gpio_enable) {
        fprintf(stderr, "Error: -l (loopback) requires -g (GPIO enable).\n");
        exit(1);
    }
    if (source_csr && !gpio_enable) {
        fprintf(stderr, "Error: -s (CSR mode) requires -g (GPIO enable).\n");
        exit(1);
    }
    if ((output_data != 0 || output_enable != 0) && !source_csr) {
        fprintf(stderr, "Error: -o/-e (output data/enable) requires -s (CSR mode).\n");
        exit(1);
    }

    /* Open connection */
    struct m2sdr_dev *conn = m2sdr_open_dev();
    if (conn == NULL) {
        exit(1);
    }

    /* Configure GPIO */
    configure_gpio(g_dev, gpio_enable, loopback_enable, source_csr, output_data, output_enable);

    /* Close connection */
    m2sdr_close_dev(conn);

    return 0;
}
