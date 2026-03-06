/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_FLASH_H
#define M2SDR_LIB_FLASH_H

#include <stdint.h>

/* Low-level SPI flash access helpers used by m2sdr_util and other board
 * maintenance tooling. They operate on the FPGA's flash bridge, not on the
 * RF datapath. */

/* SPI Constants */
/*---------------*/

#define SPI_CTRL_START   (1 << 0)
#define SPI_CTRL_LENGTH  (1 << 8)
#define SPI_STATUS_DONE  (1 << 0)

/* Flash Commands */
/*----------------*/

#define FLASH_READ_ID_REG  0x9F

#define FLASH_READ         0x03
#define FLASH_WREN         0x06
#define FLASH_WRDI         0x04
#define FLASH_PP           0x02
#define FLASH_SE           0xD8
#define FLASH_BE           0xC7
#define FLASH_RDSR         0x05
#define FLASH_WRSR         0x01

/* Flash Status */
/*--------------*/

#define FLASH_WIP          0x01

/* Flash Geometry */
/*----------------*/

#define FLASH_SECTOR_SIZE  (1 << 16)

/* Flash Functions */
/*-----------------*/

/* Read one byte from the SPI flash memory map. */
uint8_t m2sdr_flash_read(void *conn, uint32_t addr);
/* Return the erase granularity used by the flash helper implementation. */
int m2sdr_flash_get_erase_block_size(void *conn);
/* Program a flash region and optionally report progress through a printf-style
 * callback. The helper handles sector erases and page programming internally. */
int m2sdr_flash_write(void *conn,
                      uint8_t *buf, uint32_t base, uint32_t size,
                      void (*progress_cb)(void *opaque, const char *fmt, ...),
                      void *opaque);

#endif /* M2SDR_LIB_FLASH_H */
