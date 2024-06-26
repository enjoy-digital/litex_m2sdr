/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LitePCIe driver
 *
 * This file is part of LitePCIe.
 *
 * Copyright (C) 2018-2023 / EnjoyDigital  / florent@enjoy-digital.fr
 *
 */

#ifndef __HW_FLAGS_H
#define __HW_FLAGS_H

/* spi */
#define SPI_CTRL_START  (1 << 0)
#define SPI_CTRL_LENGTH (1 << 8)
#define SPI_STATUS_DONE (1 << 0)

/* pcie */
#define DMA_TABLE_LOOP_INDEX (1 <<  0)
#define DMA_TABLE_LOOP_COUNT (1 << 16)

#endif /* __HW_FLAGS_H */
