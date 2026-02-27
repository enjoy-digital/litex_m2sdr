#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.gpio import GPIORXPacker, GPIOTXUnpacker


def test_gpio_rx_packer():
    dut = GPIORXPacker()
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.i1.eq(0xA)
        yield dut.i2.eq(0x5)
        for _ in range(3):
            yield
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x123456789ABCDEF0)
        yield
        yield dut.sink.valid.eq(0)
        for _ in range(2):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()], clocks={"sys": 10, "rfic": 10})
    assert len(out) == 1
    packed = out[0]
    assert ((packed >> 12) & 0xF) == 0xA
    assert ((packed >> 44) & 0xF) == 0x5
    assert ((packed >> 28) & 0xF) == 0x0
    assert ((packed >> 60) & 0xF) == 0x0


def test_gpio_tx_unpacker():
    dut = GPIOTXUnpacker()
    captured = {}

    def gen():
        yield dut.enable.eq(1)
        for _ in range(3):
            yield
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        # IA[15:12]=1, QA[15:12]=2, IB[15:12]=3, QB[15:12]=4
        yield dut.sink.data.eq((0x4 << 60) | (0x3 << 44) | (0x2 << 28) | (0x1 << 12))
        yield
        yield
        captured["o1"] = (yield dut.o1)
        captured["oe1"] = (yield dut.oe1)
        captured["o2"] = (yield dut.o2)
        captured["oe2"] = (yield dut.oe2)
        yield dut.sink.valid.eq(0)
        yield

    run_simulation(dut, gen(), clocks={"sys": 10, "rfic": 10})

    assert captured["o1"] == 0x1
    assert captured["oe1"] == 0x2
    assert captured["o2"] == 0x3
    assert captured["oe2"] == 0x4


def test_gpio_rx_packer_disable_passthrough():
    dut = GPIORXPacker()
    out = []
    data_in = 0xFEDCBA9876543210

    def gen():
        yield dut.enable.eq(0)
        yield dut.i1.eq(0xF)
        yield dut.i2.eq(0xF)
        for _ in range(3):
            yield
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(data_in)
        yield
        yield dut.sink.valid.eq(0)
        yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()], clocks={"sys": 10, "rfic": 10})
    assert out == [data_in]


def test_gpio_tx_unpacker_disable_forces_zero_outputs():
    dut = GPIOTXUnpacker()
    observed = {}

    def gen():
        yield dut.enable.eq(0)
        for _ in range(3):
            yield
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq((0xF << 60) | (0xE << 44) | (0xD << 28) | (0xC << 12))
        yield
        yield
        observed["o1"] = (yield dut.o1)
        observed["oe1"] = (yield dut.oe1)
        observed["o2"] = (yield dut.o2)
        observed["oe2"] = (yield dut.oe2)

    run_simulation(dut, gen(), clocks={"sys": 10, "rfic": 10})
    assert observed == {"o1": 0, "oe1": 0, "o2": 0, "oe2": 0}
