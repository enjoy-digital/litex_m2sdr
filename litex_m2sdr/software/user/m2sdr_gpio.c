/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR GPIO Utility.
 *
 * This file is part of LiteX-M2SDR project.
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

/* Variables */
/*-----------*/

#ifdef USE_LITEPCIE
static char m2sdr_device[1024];
static int m2sdr_device_num = 0;
#elif defined(USE_LITEETH)
static char m2sdr_ip_address[1024] = "192.168.1.50";
static char m2sdr_port[16] = "1234";
#endif

/* Connection Functions */
/*----------------------*/

static void * m2sdr_open(void) {
#ifdef USE_LITEPCIE
    int fd = open(m2sdr_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }
    return (void *)(intptr_t)fd;
#elif defined(USE_LITEETH)
    struct eb_connection *eb = eb_connect(m2sdr_ip_address, m2sdr_port, 1);
    if (!eb) {
        fprintf(stderr, "Failed to connect to %s:%s\n", m2sdr_ip_address, m2sdr_port);
        exit(1);
    }
    return eb;
#endif
}

static void m2sdr_close(void *conn) {
#ifdef USE_LITEPCIE
    close((int)(intptr_t)conn);
#elif defined(USE_LITEETH)
    eb_disconnect((struct eb_connection **)&conn);
#endif
}

/* GPIO Control Functions */
/*-----------------------*/

static void configure_gpio(void *conn, uint8_t gpio_enable, uint8_t loopback_enable, uint8_t source_csr, uint32_t output_data, uint32_t output_enable) {
#ifdef CSR_GPIO_BASE
    uint32_t control = 0;

    /* Read current control register value */
    control = m2sdr_readl(conn, CSR_GPIO_CONTROL_ADDR);

    /* Modify control register */
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
    m2sdr_writel(conn, CSR_GPIO_CONTROL_ADDR, control);

    /* Set output data and enable if CSR mode is requested */
    if (gpio_enable && source_csr) {
        m2sdr_writel(conn, CSR_GPIO__O_ADDR,  output_data  & 0xF); /* 4-bit output data */
        m2sdr_writel(conn, CSR_GPIO_OE_ADDR,  output_enable & 0xF); /* 4-bit output enable */
    }

    /* Read and display current GPIO input */
    uint32_t input_data = m2sdr_readl(conn, CSR_GPIO__I_ADDR) & 0xF; /* 4-bit input data */

    /* Display configuration */
    printf("GPIO Control: %s, Source: %s, Loopback: %s\n",
           gpio_enable ? "Enabled" : "Disabled",
           gpio_enable && source_csr ? "CSR" : "DMA",
           gpio_enable && loopback_enable ? "Enabled" : "Disabled");
    printf("GPIO Output Data: 0x%01x, Output Enable: 0x%01x, Input Data: 0x%01x\n",
           gpio_enable && source_csr ? (output_data & 0xF) : 0,
           gpio_enable && source_csr ? (output_enable & 0xF) : 0,
           input_data);

#endif
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
        #ifdef USE_LITEPCIE
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
        #endif
        #ifdef USE_LITEETH
        case 'i':
            strncpy(m2sdr_ip_address, optarg, sizeof(m2sdr_ip_address) - 1);
            m2sdr_ip_address[sizeof(m2sdr_ip_address) - 1] = '\0';
            break;
        case 'p':
            strncpy(m2sdr_port, optarg, sizeof(m2sdr_port) - 1);
            m2sdr_port[sizeof(m2sdr_port) - 1] = '\0';
            break;
        #endif
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
    #ifdef USE_LITEPCIE
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);
    #endif

    /* Open connection */
    void *conn = m2sdr_open();
    if (conn == NULL) {
        exit(1);
    }

    /* Configure GPIO */
    configure_gpio(conn, gpio_enable, loopback_enable, source_csr, output_data, output_enable);

    /* Close connection */
    m2sdr_close(conn);

    return 0;
}
