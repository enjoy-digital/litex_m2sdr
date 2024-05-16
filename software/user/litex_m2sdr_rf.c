/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LitePCIe rf
 *
 * This file is part of LitePCIe.
 *
 * Copyright (C) 2018-2023 / EnjoyDigital  / florent@enjoy-digital.fr
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

#include "liblitepcie.h"
#include "liblitexm2sdr.h"

/* Parameters */
/*------------*/

/* Variables */
/*-----------*/

static char litepcie_device[1024];
static int litepcie_device_num;

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* Info */
/*------*/

static void info(void)
{
    int fd;
    int i;
    unsigned char fpga_identifier[256];

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }


    printf("\e[1m[> FPGA/SoC Info:\e[0m\n");
    printf("---------------------\n");

    for (i = 0; i < 256; i ++)
        fpga_identifier[i] = litepcie_readl(fd, CSR_IDENTIFIER_MEM_BASE + 4 * i);
    printf("FPGA Identifier:  %s.\n", fpga_identifier);
#ifdef CSR_DNA_BASE
    printf("FPGA DNA:         0x%08x%08x\n",
        litepcie_readl(fd, CSR_DNA_ID_ADDR + 4 * 0),
        litepcie_readl(fd, CSR_DNA_ID_ADDR + 4 * 1)
    );
#endif
#ifdef CSR_XADC_BASE
    printf("FPGA Temperature : %0.1f °C\n",
           (double)litepcie_readl(fd, CSR_XADC_TEMPERATURE_ADDR) * 503.975/4096 - 273.15);
    printf("FPGA VCC-INT     : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCINT_ADDR) / 4096 * 3);
    printf("FPGA VCC-AUX     : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCAUX_ADDR) / 4096 * 3);
    printf("FPGA VCC-BRAM    : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3);
#endif
    printf("\n");

    close(fd);
}

/* Help */
/*------*/

static void help(void)
{
    printf("LiteXM2SDR utilities\n"
           "usage: litex_m2sdr_rf [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "-h                                Help.\n"
           "-c device_num                     Select the device (default = 0).\n"
           "\n"
           "available commands:\n"
           "info                              Get Board information.\n"
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    const char *cmd;
    int c;

    litepcie_device_num = 0;

    /* Parameters. */
    for (;;) {
        c = getopt(argc, argv, "hc:w:zea");
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        default:
            exit(1);
        }
    }

    /* Show help when too much args. */
    if (optind >= argc)
        help();

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/litepcie%d", litepcie_device_num);

    cmd = argv[optind++];

    /* Info cmds. */
    if (!strcmp(cmd, "info"))
        info();
    /* Show help otherwise. */
    else
        goto show_help;

    return 0;

show_help:
        help();

    return 0;
}
