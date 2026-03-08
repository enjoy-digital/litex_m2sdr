#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

import sys
import types

from litex.gen.sim import run_simulation

if "litei2c" not in sys.modules:
    litei2c_stub = types.ModuleType("litei2c")
    litei2c_stub.LiteI2C = object
    sys.modules["litei2c"] = litei2c_stub

from litex_m2sdr.gateware.si5351 import LiteI2CSequencer


def test_litei2c_sequencer_writes_full_sequence_and_finishes():
    """Verify the sequencer emits each configured word to the RXTX register and reaches done."""
    seq = [(0x10, 0xAA), (0x11, 0x55), (0x12, 0x77)]
    dut = LiteI2CSequencer(
        sys_clk_freq=100e6,
        i2c_base=0x1000,
        i2c_adr=0x60,
        i2c_sequence=seq,
    )
    writes = []
    samples = {}

    I2C_MASTER_ACTIVE_ADDR   = 0x1000 + 0x04
    I2C_MASTER_SETTINGS_ADDR = 0x1000 + 0x08
    I2C_MASTER_ADDR_ADDR     = 0x1000 + 0x0C
    I2C_MASTER_RXTX_ADDR     = 0x1000 + 0x10
    I2C_MASTER_STATUS_ADDR   = 0x1000 + 0x14

    def gen():
        for _ in range(80):
            yield
        samples["done"] = (yield dut.done)

    @passive
    def wb_responder():
        while True:
            yield dut.bus.ack.eq(0)
            if (yield dut.bus.cyc) and (yield dut.bus.stb):
                adr = (yield dut.bus.adr)
                we  = (yield dut.bus.we)
                if not we and adr == I2C_MASTER_STATUS_ADDR:
                    yield dut.bus.dat_r.eq(0b01)  # TX ready, RX empty.
                elif we and adr == I2C_MASTER_RXTX_ADDR:
                    writes.append((adr, (yield dut.bus.dat_w)))
                elif we and adr in {I2C_MASTER_ACTIVE_ADDR, I2C_MASTER_SETTINGS_ADDR, I2C_MASTER_ADDR_ADDR}:
                    writes.append((adr, (yield dut.bus.dat_w)))
                yield dut.bus.ack.eq(1)
            yield

    run_simulation(dut, [gen(), wb_responder()])

    expected_words = [(addr << 8) | data for addr, data in seq]
    rxtx_writes = [data for adr, data in writes if adr == I2C_MASTER_RXTX_ADDR]
    assert rxtx_writes == expected_words
    assert samples["done"] == 1


def test_litei2c_sequencer_flushes_rx_before_transmit():
    """Verify the sequencer performs an RX flush when status reports pending RX data."""
    seq = [(0x20, 0x01), (0x21, 0x02)]
    dut = LiteI2CSequencer(
        sys_clk_freq=100e6,
        i2c_base=0x2000,
        i2c_adr=0x60,
        i2c_sequence=seq,
    )
    accesses = []

    I2C_MASTER_RXTX_ADDR     = 0x2000 + 0x10
    I2C_MASTER_STATUS_ADDR   = 0x2000 + 0x14
    rx_pending = {"count": 1}

    def gen():
        for _ in range(40):
            yield

    @passive
    def wb_responder():
        while True:
            yield dut.bus.ack.eq(0)
            if (yield dut.bus.cyc) and (yield dut.bus.stb):
                adr = (yield dut.bus.adr)
                we  = (yield dut.bus.we)
                accesses.append((adr, we))
                if not we and adr == I2C_MASTER_STATUS_ADDR:
                    if rx_pending["count"] > 0:
                        yield dut.bus.dat_r.eq(0b10)  # RX pending, TX not yet ready.
                    else:
                        yield dut.bus.dat_r.eq(0b01)  # TX ready.
                if not we and adr == I2C_MASTER_RXTX_ADDR:
                    rx_pending["count"] = 0
                yield dut.bus.ack.eq(1)
            yield

    run_simulation(dut, [gen(), wb_responder()])

    assert (I2C_MASTER_RXTX_ADDR, 0) in accesses
    assert (I2C_MASTER_RXTX_ADDR, 1) in accesses
