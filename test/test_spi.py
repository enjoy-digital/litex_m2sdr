#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ad9361.spi import AD9361SPIMaster


class _SPIPads:
    def __init__(self):
        self.cs_n = Signal(reset=1)
        self.clk  = Signal()
        self.mosi = Signal()
        self.miso = Signal()


def _bit_reverse(value, width):
    out = 0
    for i in range(width):
        out = (out << 1) | ((value >> i) & 0x1)
    return out


def _run_write(csr, value):
    return csr.write(value)


def test_ad9361_spi_master_completes_transfer_and_deasserts_chip_select():
    """Verify a CSR-triggered transfer completes and chip select returns inactive."""
    pads = _SPIPads()
    dut = AD9361SPIMaster(pads=pads, data_width=8, clk_divider=4)
    samples = {}

    def gen():
        yield from _run_write(dut._mosi, 0xA5)
        yield from _run_write(dut._control, (8 << 8) | 0x1)
        for _ in range(64):
            yield
        samples["done"] = (yield dut._status.fields.done)
        samples["cs_n"] = (yield pads.cs_n)

    run_simulation(dut, gen())

    assert samples["done"] == 1
    assert samples["cs_n"] == 1


def test_ad9361_spi_master_captures_miso_bits():
    """Verify MISO samples are shifted into the exposed status register with current capture latency."""
    pads = _SPIPads()
    dut = AD9361SPIMaster(pads=pads, data_width=8, clk_divider=4)
    miso_pattern = 0x96
    observed = {}

    def gen():
        yield from _run_write(dut._mosi, 0x00)
        yield from _run_write(dut._control, (8 << 8) | 0x1)
        for _ in range(96):
            yield
        observed["miso"] = (yield dut._miso.status)

    @passive
    def miso_driver():
        bit_index = 7
        last_clk = 0
        while True:
            clk = (yield pads.clk)
            cs_n = (yield pads.cs_n)
            if not cs_n and last_clk == 1 and clk == 0 and bit_index >= 0:
                yield pads.miso.eq((miso_pattern >> bit_index) & 0x1)
                bit_index -= 1
            last_clk = clk
            yield

    run_simulation(dut, [gen(), miso_driver()])

    assert observed["miso"] & 0xFF == (miso_pattern >> 1)


def test_ad9361_spi_master_respects_programmed_length():
    """Verify shorter transfers only shift the programmed number of bits."""
    pads = _SPIPads()
    dut = AD9361SPIMaster(pads=pads, data_width=8, clk_divider=4)
    observed = {}

    def gen():
        yield from _run_write(dut._mosi, 0xF0)
        yield from _run_write(dut._control, (4 << 8) | 0x1)
        for _ in range(48):
            yield
        observed["done"] = (yield dut._status.fields.done)
        observed["miso"] = (yield dut._miso.status)

    @passive
    def miso_driver():
        pattern = 0b1010
        bit_index = 3
        last_clk = 0
        while True:
            clk = (yield pads.clk)
            cs_n = (yield pads.cs_n)
            if not cs_n and last_clk == 1 and clk == 0 and bit_index >= 0:
                yield pads.miso.eq((pattern >> bit_index) & 0x1)
                bit_index -= 1
            last_clk = clk
            yield

    run_simulation(dut, [gen(), miso_driver()])

    assert observed["done"] == 1
    assert observed["miso"] & 0xF == _bit_reverse(0b1010, 4)
