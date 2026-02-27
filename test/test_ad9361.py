#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ad9361.bitmode import AD9361RXBitMode, AD9361TXBitMode
from litex_m2sdr.gateware.ad9361.prbs import AD9361PRBSChecker, AD9361PRBSGenerator


def test_ad9361_tx_bitmode_16bit_passthrough():
    dut = AD9361TXBitMode()
    out = []

    def gen():
        yield dut.mode.eq(0)
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x1122334455667788)
        yield
        yield dut.sink.valid.eq(0)
        yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert out == [0x1122334455667788]


def test_ad9361_rx_bitmode_16bit_passthrough():
    dut = AD9361RXBitMode()
    out = []

    def gen():
        yield dut.mode.eq(0)
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x99AABBCCDDEEFF00)
        yield
        yield dut.sink.valid.eq(0)
        yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert out == [0x99AABBCCDDEEFF00]


def test_ad9361_prbs_checker_sync_and_loss():
    gen = AD9361PRBSGenerator(seed=0x0A54)
    chk = AD9361PRBSChecker(seed=0x0A54)
    dut = Module()
    dut.submodules += gen, chk
    synced_trace = []

    def stimulus():
        for _ in range(1200):
            yield chk.i.eq((yield gen.o) & 0xFFF)
            synced_trace.append((yield chk.synced))
            yield
        # Inject wrong samples to force loss of sync.
        for _ in range(4):
            yield chk.i.eq(0x000)
            synced_trace.append((yield chk.synced))
            yield

    run_simulation(dut, stimulus())
    assert any(synced_trace[1100:])
    assert synced_trace[-1] == 0
