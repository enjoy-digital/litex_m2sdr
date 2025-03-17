/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_AD9361_SPI_H
#define M2SDR_LIB_AD9361_SPI_H

#include <stdbool.h>

#include "csr.h"
#include "soc.h"

#include "etherbone.h"

/* SPI Constants */
/*---------------*/

#define SPI_CONTROL_START  (1 << 0)
#define SPI_CONTROL_LENGTH (1 << 8)
#define SPI_STATUS_DONE    (1 << 0)

/* PCIe SPI functions */
/*---------------*/

void m2sdr_ad9361_spi_init(int fd, uint8_t reset);
void m2sdr_ad9361_spi_xfer(int fd, uint8_t len, uint8_t *mosi, uint8_t *miso);
void m2sdr_ad9361_spi_write(int fd, uint16_t reg, uint8_t dat);
uint8_t m2sdr_ad9361_spi_read(int fd, uint16_t reg);

/* Etherbone SPI functions */
/*-------------------------*/

void m2sdr_ad9361_eb_spi_init(struct eb_connection *fd, uint8_t reset);
void m2sdr_ad9361_eb_spi_xfer(struct eb_connection *eb, uint8_t len, uint8_t *mosi, uint8_t *miso);
void m2sdr_ad9361_eb_spi_write(struct eb_connection *eb, uint16_t reg, uint8_t dat);
uint8_t m2sdr_ad9361_eb_spi_read(struct eb_connection *eb, uint16_t reg);

#endif /* M2SDR_LIB_AD9361_SPI_H */
