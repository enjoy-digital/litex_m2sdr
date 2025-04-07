/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR GPIO Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>

#include "liblitepcie.h"

/* GPIO Control Functions */
/*-----------------------*/

static void configure_gpio(int fd, uint8_t gpio_enable, uint8_t loopback_enable, uint8_t source_csr, uint32_t output_data, uint32_t output_enable) {
    uint32_t control = 0;

    /* Read current control register value */
    control = litepcie_readl(fd, CSR_GPIO_CONTROL_ADDR);

    /* Modify control register */
    printf("gpio_enable: %x\n", gpio_enable);
    printf("control: %x\n", control);
    if (gpio_enable) {
        control |= (1 << CSR_GPIO_CONTROL_ENABLE_OFFSET);  /* Enable GPIO */
        if (loopback_enable) {
            control |= (1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET);  /* Enable loopback */
        } else {
            control &= ~(1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET); /* Disable loopback */
        }
        if (source_csr) {
            control |= (1 << CSR_GPIO_CONTROL_SOURCE_OFFSET);  /* CSR mode */
        } else {
            control &= ~(1 << CSR_GPIO_CONTROL_SOURCE_OFFSET); /* Packer/Unpacker mode */
        }
    } else {
        control &= ~(1 << CSR_GPIO_CONTROL_ENABLE_OFFSET); /* Disable GPIO */
    }
    printf("control: %x\n", control);
    litepcie_writel(fd, CSR_GPIO_CONTROL_ADDR, control);

    /* Set output data and enable if CSR mode is requested */
    if (gpio_enable && source_csr) {
        litepcie_writel(fd, CSR_GPIO__O_ADDR, output_data & 0xF);   /* 4-bit output data */
        litepcie_writel(fd, CSR_GPIO_OE_ADDR, output_enable & 0xF); /* 4-bit output enable */
    }

    /* Read and display current GPIO input */
    uint32_t input_data = litepcie_readl(fd, CSR_GPIO__I_ADDR) & 0xF; /* 4-bit input data */

    /* Display configuration */
    printf("GPIO Control: %s, Source: %s, Loopback: %s\n",
           gpio_enable ? "Enabled" : "Disabled",
           gpio_enable && source_csr ? "CSR" : "DMA",
           gpio_enable && loopback_enable ? "Enabled" : "Disabled");
    printf("GPIO Output Data: 0x%01x, Output Enable: 0x%01x, Input Data: 0x%01x\n",
           gpio_enable && source_csr ? (output_data & 0xF) : 0,
           gpio_enable && source_csr ? (output_enable & 0xF) : 0,
           input_data);
}

/* Help */
/*------*/

static void help(void) {
    printf("M2SDR GPIO Utility\n"
           "usage: m2sdr_gpio [options]\n"
           "\n"
           "Options:\n"
           "-h                    Display this help message.\n"
           "-c device_num         Select the device (default = 0).\n"
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
    static char litepcie_device[1024];
    static int litepcie_device_num = 0;
    static uint8_t gpio_enable = 0;
    static uint8_t loopback_enable = 0;
    static uint8_t source_csr = 0;
    static uint32_t output_data = 0;
    static uint32_t output_enable = 0;

    /* Parameters */
    for (;;) {
        c = getopt(argc, argv, "hc:glso:e:");
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
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

    /* Select device */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/m2sdr%d", litepcie_device_num);

    /* Open device */
    int fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        exit(1);
    }

    /* Configure GPIO */
    configure_gpio(fd, gpio_enable, loopback_enable, source_csr, output_data, output_enable);

    /* Close device */
    close(fd);

    return 0;
}