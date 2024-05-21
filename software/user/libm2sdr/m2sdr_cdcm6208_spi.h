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

#endif /* M2SDR_LIB_CDCM6208_SPI_H */
