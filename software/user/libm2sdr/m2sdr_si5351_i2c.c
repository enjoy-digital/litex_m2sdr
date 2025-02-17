/*
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>

#include "liblitepcie.h"

#include "m2sdr_si5351_i2c.h"

#ifdef CSR_SI5351_I2C_W_ADDR

extern void nanosleep(int n);

/* Private Functions */

static void si5351_i2c_delay(int n) {
	nanosleep(n);
}

static inline void si5351_i2c_oe_scl_sda(int fd, bool oe, bool scl, bool sda)
{
	litepcie_writel(fd, CSR_SI5351_I2C_W_ADDR,
		((oe & 1)  << CSR_SI5351_I2C_W_OE_OFFSET)	|
		((scl & 1) << CSR_SI5351_I2C_W_SCL_OFFSET) |
		((sda & 1) << CSR_SI5351_I2C_W_SDA_OFFSET)
	);
}

static void si5351_i2c_start(int fd)
{
	si5351_i2c_oe_scl_sda(fd, 1, 1, 1);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 1, 1, 0);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 1, 0, 0);
	si5351_i2c_delay(1);
}

static void si5351_i2c_stop(int fd)
{
	si5351_i2c_oe_scl_sda(fd, 1, 0, 0);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 1, 1, 0);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 1, 1, 1);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 0, 1, 1);
}

static void si5351_i2c_transmit_bit(int fd, int value)
{
	si5351_i2c_oe_scl_sda(fd, 1, 0, value);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 1, 1, value);
	si5351_i2c_delay(2);
	si5351_i2c_oe_scl_sda(fd, 1, 0, value);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 0, 0, 0);
}

static int si5351_i2c_receive_bit(int fd)
{
	int value;
	si5351_i2c_oe_scl_sda(fd, 0, 0, 0);
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 0, 1, 0);
	si5351_i2c_delay(1);
	value = litepcie_readl(fd, CSR_SI5351_I2C_R_ADDR) & 1;
	si5351_i2c_delay(1);
	si5351_i2c_oe_scl_sda(fd, 0, 0, 0);
	si5351_i2c_delay(1);
	return value;
}

/* Transmit byte and return 1 if slave sends ACK */

static bool si5351_i2c_transmit_byte(int fd, uint8_t data)
{
	int i;
	int ack;

	/* SCL should have already been low for 1/4 cycle */
	si5351_i2c_oe_scl_sda(fd, 0, 0, 0);
	for (i = 0; i < 8; ++i) {
		/* MSB first */
		si5351_i2c_transmit_bit(fd, (data & (1 << 7)) != 0);
		data <<= 1;
	}
	ack = si5351_i2c_receive_bit(fd);

	/* 0 from Slave means ACK */
	return ack == 0;
}

/* Receive byte and send ACK if ack=1 */
static uint8_t si5351_i2c_receive_byte(int fd, bool ack)
{
	int i;
	uint8_t data = 0;

	for (i = 0; i < 8; ++i) {
		data <<= 1;
		data |= si5351_i2c_receive_bit(fd);
	}
	si5351_i2c_transmit_bit(fd, !ack);

	return data;
}

/* Reset line state */
void m2sdr_si5351_i2c_reset(int fd)
{
	int i;
	si5351_i2c_oe_scl_sda(fd, 1, 1, 1);
	si5351_i2c_delay(8);
	for (i = 0; i < 9; ++i) {
		si5351_i2c_oe_scl_sda(fd, 1, 0, 1);
		si5351_i2c_delay(2);
		si5351_i2c_oe_scl_sda(fd, 1, 1, 1);
		si5351_i2c_delay(2);
	}
	si5351_i2c_oe_scl_sda(fd, 0, 0, 1);
	si5351_i2c_delay(1);
	si5351_i2c_stop(fd);
	si5351_i2c_oe_scl_sda(fd, 0, 1, 1);
	si5351_i2c_delay(8);
}

/* Public Functions */

/*
 * Read slave memory over I2C starting at given address
 *
 * First writes the memory starting address, then reads the data:
 *   START WR(slaveaddr) WR(addr) STOP START WR(slaveaddr) RD(data) RD(data) ... STOP
 * Some chips require that after transmiting the address, there will be no STOP in between:
 *   START WR(slaveaddr) WR(addr) START WR(slaveaddr) RD(data) RD(data) ... STOP
 */
bool m2sdr_si5351_i2c_read(int fd, uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop)
{
	int i;

	si5351_i2c_start(fd);

	if(!si5351_i2c_transmit_byte(fd, SI5351_I2C_ADDR_WR(slave_addr))) {
		si5351_i2c_stop(fd);
		return false;
	}
	if(!si5351_i2c_transmit_byte(fd, addr)) {
		si5351_i2c_stop(fd);
		return false;
	}

	if (send_stop) {
		si5351_i2c_stop(fd);
	}
	si5351_i2c_start(fd);

	if(!si5351_i2c_transmit_byte(fd, SI5351_I2C_ADDR_RD(slave_addr))) {
		si5351_i2c_stop(fd);
		return false;
	}
	for (i = 0; i < len; ++i) {
		data[i] = si5351_i2c_receive_byte(fd, i != (len - 1));
	}

	si5351_i2c_stop(fd);

	return true;
}

/*
 * Write slave memory over I2C starting at given address
 *
 * First writes the memory starting address, then writes the data:
 *   START WR(slaveaddr) WR(addr) WR(data) WR(data) ... STOP
 */
bool m2sdr_si5351_i2c_write(int fd, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len)
{
	int i;

	si5351_i2c_start(fd);

	if(!si5351_i2c_transmit_byte(fd, SI5351_I2C_ADDR_WR(slave_addr))) {
		si5351_i2c_stop(fd);
		return false;
	}
	if(!si5351_i2c_transmit_byte(fd, addr)) {
		si5351_i2c_stop(fd);
		return false;
	}
	for (i = 0; i < len; ++i) {
		if(!si5351_i2c_transmit_byte(fd, data[i])) {
			si5351_i2c_stop(fd);
			return false;
		}
	}

	si5351_i2c_stop(fd);

	return true;
}

/*
 * Poll I2C slave at given address, return true if it sends an ACK back
 */
bool m2sdr_si5351_i2c_poll(int fd, uint8_t slave_addr)
{
    bool result;

    si5351_i2c_start(fd);
    result  = si5351_i2c_transmit_byte(fd, SI5351_I2C_ADDR_WR(slave_addr));
    result |= si5351_i2c_transmit_byte(fd, SI5351_I2C_ADDR_RD(slave_addr));
    si5351_i2c_stop(fd);

    return result;
}

/*
 * Scan I2C
 */
void m2sdr_si5351_i2c_scan(int fd)
{
	int slave_addr;

	printf("       0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
	for (slave_addr = 0; slave_addr < 0x80; slave_addr++) {
		if (slave_addr % 0x10 == 0) {
			printf("\n0x%02x:", slave_addr & 0x70);
		}
		if (m2sdr_si5351_i2c_poll(fd, slave_addr)) {
			printf(" %02x", slave_addr);
		} else {
			printf(" --");
		}
	}
	printf("\n");
}

/*
 * I2C Config
 */

void m2sdr_si5351_i2c_config(int fd, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length)
{
	int i;
    for (i=0; i<i2c_length; i++) {
        uint8_t addr = i2c_config[i][0];
        uint8_t data = i2c_config[i][1];
        if (!m2sdr_si5351_i2c_write(fd, i2c_addr, addr, &data, 1)) {
            fprintf(stderr, "Failed to write to SI5351 at register 0x%02X\n", addr);
        }
    }
}


#endif