/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_AD9361_SPI_H
#define M2SDR_LIB_AD9361_SPI_H

#include <stdbool.h>

#include "csr.h"
#include "soc.h"

#include "etherbone.h"

/* This header exposes the low-level AD9361 SPI bridge used by the RF layer.
 * Most applications should not call these helpers directly; prefer the higher
 * level RF API in m2sdr.h unless you are debugging or extending bring-up. */

/* SPI Constants */
/*---------------*/

#define SPI_CONTROL_START  (1 << 0)
#define SPI_CONTROL_LENGTH (1 << 8)
#define SPI_STATUS_DONE    (1 << 0)

/* SPI functions */
/*---------------*/

/* Reset and enable the FPGA-side AD9361 SPI engine. */
void m2sdr_ad9361_spi_init(void *conn, uint8_t reset);
/* Submit one raw 24-bit AD9361 SPI transaction. */
void m2sdr_ad9361_spi_xfer(void *conn, uint8_t len, const uint8_t *mosi, uint8_t *miso);
/* Write an 8-bit AD9361 register value. */
void m2sdr_ad9361_spi_write(void *conn, uint16_t reg, uint8_t dat);
/* Read an 8-bit AD9361 register value. */
uint8_t m2sdr_ad9361_spi_read(void *conn, uint16_t reg);

#endif /* M2SDR_LIB_AD9361_SPI_H */
