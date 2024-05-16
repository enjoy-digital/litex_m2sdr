/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __LITEXM2SDR_SI5351_I2C_H
#define __LITEXM2SDR_SI5351_I2C_H

#include <stdbool.h>

#include "csr.h"
#include "soc.h"

#define SI5351_I2C_ADDR_WR(addr)  ((addr) << 1)
#define SI5351_I2C_ADDR_RD(addr) (((addr) << 1) | 1u)

void litexm2sdr_si5351_i2c_reset(int fd);
bool litexm2sdr_si5351_i2c_write(int fd, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len);
bool litexm2sdr_si5351_i2c_read(int fd,  uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop);
bool litexm2sdr_si5351_i2c_poll(int fd,  uint8_t slave_addr);
void litexm2sdr_si5351_i2c_scan(int fd);

#endif /* __LITEXM2SDR_SI5351_I2C_H */
