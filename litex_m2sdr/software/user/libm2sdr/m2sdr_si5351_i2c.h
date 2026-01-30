/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef M2SDR_LIB_SI5351_I2C_H
#define M2SDR_LIB_SI5351_I2C_H

#include <stdbool.h>

#include "csr.h"
#include "soc.h"

/* I2C Constants */
/*---------------*/

#define SI5351_I2C_ADDR_WR(addr)  ((addr) << 1)
#define SI5351_I2C_ADDR_RD(addr) (((addr) << 1) | 1u)

/* Unified I2C functions (PCIe or Etherbone) */
/*--------------------------------------------*/

void m2sdr_si5351_i2c_reset(void *conn);
bool m2sdr_si5351_i2c_write(void *conn, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len);
bool m2sdr_si5351_i2c_read(void *conn,  uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop);
bool m2sdr_si5351_i2c_poll(void *conn,  uint8_t slave_addr);
bool m2sdr_si5351_i2c_check_litei2c(void *conn);
void m2sdr_si5351_i2c_config(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length);

#endif /* M2SDR_LIB_SI5351_I2C_H */
