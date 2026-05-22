/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_H
#define M2SDR_LIB_H

#include "liblitepcie.h"
#include "etherbone.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Constants */

/* ICAP */
#define ICAP_CMD_REG   0b00100
#define ICAP_CMD_IPROG 0b01111

#define ICAP_IDCODE_REG   0b01100

#define ICAP_BOOTSTS_REG  0b10110
#define ICAP_BOOTSTS_VALID    (1 << 0)
#define ICAP_BOOTSTS_FALLBACK (1 << 1)

/* Legacy raw-handle helpers. */

static inline int m2sdr_legacy_handle_is_fd(void *conn)
{
    intptr_t value = (intptr_t)conn;

    return value >= 0 && value < 1048576;
}

static inline void m2sdr_writel(void *conn, uint32_t addr, uint32_t val)
{
    if (m2sdr_legacy_handle_is_fd(conn))
        litepcie_writel((int)(intptr_t)conn, addr, val);
    else
        eb_write32((struct eb_connection *)conn, val, addr);
}

static inline uint32_t m2sdr_readl(void *conn, uint32_t addr)
{
    if (m2sdr_legacy_handle_is_fd(conn))
        return litepcie_readl((int)(intptr_t)conn, addr);
    return eb_read32((struct eb_connection *)conn, addr);
}

/* Libs */

#include "m2sdr_si5351_i2c.h"
#include "m2sdr_ad9361_spi.h"
#include "m2sdr_flash.h"

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_LIB_H */
