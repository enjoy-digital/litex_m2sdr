/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_H
#define M2SDR_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Constants */

/* Macros */

#ifdef USE_LITEPCIE

#define m2sdr_conn_type int
#define m2sdr_conn_cast(conn) ((m2sdr_conn_type)(intptr_t)(conn))
#define m2sdr_writel(conn, addr, val) litepcie_writel(m2sdr_conn_cast(conn), addr, val)
#define m2sdr_readl(conn, addr)       litepcie_readl(m2sdr_conn_cast(conn), addr)

#elif USE_LITEETH

#define m2sdr_conn_type struct eb_connection *
#define m2sdr_conn_cast(conn) ((m2sdr_conn_type)(conn))
#define m2sdr_writel(conn, addr, val) eb_write32(m2sdr_conn_cast(conn), val, addr)
#define m2sdr_readl(conn, addr)       eb_read32(m2sdr_conn_cast(conn), addr)

#else

#error "Define USE_LITEPCIE or USE_LITEETH for build configuration"

#endif

/* Libs */

#include "m2sdr_si5351_i2c.h"
#include "m2sdr_ad9361_spi.h"
#include "m2sdr_flash.h"

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_LIB_H */
