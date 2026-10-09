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
#include <stddef.h>
#include <stdint.h>

#include "csr.h"
#include "soc.h"

/* These helpers talk to the FPGA-hosted LiteI2C master that programs the
 * SI5351 clock generator during RF initialization. */

/* I2C Constants */
/*---------------*/

#define SI5351_I2C_ADDR_WR(addr)  ((addr) << 1)
#define SI5351_I2C_ADDR_RD(addr) (((addr) << 1) | 1u)

/* PLLB feedback Multisynth register block (AN619 registers 34..41). */
#define SI5351_PLLB_FB_NUM_REGS 8

/* Snapshot of the PLLB feedback register block (0x22..0x29) last written to
 * the device, used to minimize and classify retrim writes. */
struct m2sdr_si5351_pllb_state {
    bool    valid;
    uint8_t regs[SI5351_PLLB_FB_NUM_REGS];
};

/* What a trim call did to the device. A glitch-free update moved only the
 * fractional P2 field; a re-centred one rewrote P1 and/or P3 and passed
 * through off-frequency intermediate states. */
struct m2sdr_si5351_pllb_trim_report {
    bool     glitch_free;
    bool     recentred;
    unsigned bytes_written;
};

/* Unified I2C functions (PCIe or Etherbone) */
/*--------------------------------------------*/

/* Reset the LiteI2C controller state and drain any pending RX data. */
void m2sdr_si5351_i2c_reset(void *conn);
/* Write len consecutive SI5351 registers starting at addr through the
 * LiteI2C master. A multi-register block relies on the SI5351 address
 * auto-increment and goes out as a single I2C write transaction. */
bool m2sdr_si5351_i2c_write(void *conn, uint8_t slave_addr, uint8_t addr, const uint8_t *data, uint32_t len);
/* Read a single SI5351 register through the LiteI2C master. */
bool m2sdr_si5351_i2c_read(void *conn,  uint8_t slave_addr, uint8_t addr, uint8_t *data, uint32_t len, bool send_stop);
/* Poll for the target device to respond on the I2C bus. */
bool m2sdr_si5351_i2c_poll(void *conn,  uint8_t slave_addr);
/* Detect whether the current gateware includes the required LiteI2C block. */
bool m2sdr_si5351_i2c_check_litei2c(void *conn);
/* Apply one of the predefined SI5351 register tables from m2sdr_config.h. */
bool m2sdr_si5351_i2c_config_checked(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length);
void m2sdr_si5351_i2c_config(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length);
/* Seed a PLLB state snapshot from the nominal register table, to be called
 * right after the table has been applied to the device. */
bool m2sdr_si5351_pllb_state_from_config(struct m2sdr_si5351_pllb_state *state, const uint8_t i2c_config[][2], size_t i2c_length);
/* Glitch-free trim window around the nominal table, in ppm: within
 * [*ppm_min, *ppm_max] a retrim from nominal state moves only the P2
 * fractional field. The window is table-dependent and asymmetric, and
 * positive ppm lowers the feedback multiplier. */
bool m2sdr_si5351_pllb_trim_window(const uint8_t i2c_config[][2], size_t i2c_length, double *ppm_min, double *ppm_max);
/* Rescale the PLLB feedback multiplier of a running SI5351 to compensate a
 * measured reference error in ppm. i2c_config/i2c_length must be the nominal
 * register table the device was configured with; each trim is relative to
 * it, never cumulative. With a valid state the update writes only the
 * registers that changed, keeping P1/P3 fixed and moving P2 alone whenever
 * the target stays inside the trim window. state and report may be NULL;
 * without state the full block is rewritten. */
bool m2sdr_si5351_i2c_trim_pllb_ppm(void *conn, uint8_t i2c_addr, const uint8_t i2c_config[][2], size_t i2c_length, double ppm,
                                    struct m2sdr_si5351_pllb_state *state, struct m2sdr_si5351_pllb_trim_report *report);

#endif /* M2SDR_LIB_SI5351_I2C_H */
