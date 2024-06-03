/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_CDCM6208_SPI_H
#define M2SDR_LIB_CDCM6208_SPI_H

#include <stdbool.h>

#include "csr.h"
#include "soc.h"

/* SPI Constants */
/*---------------*/

#define SPI_CONTROL_START  (1 << 0)
#define SPI_CONTROL_LENGTH (1 << 8)
#define SPI_STATUS_DONE    (1 << 0)

/* SPI functions */
/*---------------*/

void m2sdr_cdcm6208_spi_init(int fd);
void m2sdr_cdcm6208_spi_write(int fd, uint16_t reg, uint16_t dat);
uint8_t m2sdr_cdcm6208_spi_read(int fd, uint16_t reg);

/* CDCM6208 38.4MHz internal VCXO Config */
/*---------------------------------------*/
static const uint16_t cdcm6208_regs_vcxo_38p4[][2] =
{
    { 0,  0x01B9 },
    { 1,  0x0000 },
    { 2,  0x0013 },
    { 3,  0x00F0 },
    { 4,  0x30AF | (2 << 3)},
    { 5,  0x019B },
    { 6,  0x0013 },
    { 7,  0x0000 },
    { 8,  0x0013 },
    { 9,  0x0053 },
    { 10, 0x0130 },
    { 11, 0x0000 },
    { 12, 0x0053 },
    { 13, 0x0130 },
    { 14, 0x0000 },
    { 15, 0x0003 },
    { 16, 0x0130 },
    { 17, 0x0000 },
    { 18, 0x0000 },
    { 19, 0x0130 },
    { 20, 0x0000 },
};

#endif /* M2SDR_LIB_CDCM6208_SPI_H */
