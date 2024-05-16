/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __AD9361_SPI_H
#define __AD9361_SPI_H

#include <stdbool.h>

#include "csr.h"
#include "soc.h"

/* SPI Constants */
/*---------------*/

#define SPI_CONTROL_START  (1 << 0)
#define SPI_CONTROL_LENGTH (1 << 8)
#define SPI_STATUS_DONE    (1 << 0)

#define SPI_CS_MANUAL (1<<16)

#define SYS_CLK_FREQ_MHZ (CONFIG_CLOCK_FREQUENCY/1000000)
#define SPI_CLK_FREQ_MHZ 1

#define SPI_AD9361_CS 0

/* SPI functions */
/*---------------*/

void ad9361_spi_init(int fd);
void ad9361_spi_xfer(int fd, uint8_t cs, uint8_t len, uint8_t *mosi, uint8_t *miso);
void ad9361_spi_write(int fd, uint8_t cs, uint16_t reg, uint8_t dat);
uint8_t ad9361_spi_read(int fd, uint8_t cs, uint16_t reg);

#endif /* __AD9361_SPI_H */
