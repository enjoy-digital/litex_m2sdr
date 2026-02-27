#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.loopback import TXRXLoopback


def test_txrx_loopback_normal_and_loopback():
    dut = TXRXLoopback(data_width=64, with_csr=False)
    tx_out = []
    rx_out = []

    def gen():
        yield dut.tx_source.ready.eq(1)
        yield dut.rx_source.ready.eq(1)

        # Normal mode.
        yield dut.enable.eq(0)
        yield dut.tx_sink.valid.eq(1)
        yield dut.tx_sink.first.eq(1)
        yield dut.tx_sink.last.eq(1)
        yield dut.tx_sink.data.eq(0xAAAA)
        yield dut.rx_sink.valid.eq(1)
        yield dut.rx_sink.first.eq(1)
        yield dut.rx_sink.last.eq(1)
        yield dut.rx_sink.data.eq(0xBBBB)
        yield
        yield dut.tx_sink.valid.eq(0)
        yield dut.rx_sink.valid.eq(0)
        yield

        # Loopback mode.
        yield dut.enable.eq(1)
        yield dut.tx_sink.valid.eq(1)
        yield dut.tx_sink.first.eq(1)
        yield dut.tx_sink.last.eq(1)
        yield dut.tx_sink.data.eq(0xCCCC)
        yield dut.rx_sink.valid.eq(1)
        yield dut.rx_sink.data.eq(0xDDDD)  # must be ignored/drained.
        yield
        yield dut.tx_sink.valid.eq(0)
        yield dut.rx_sink.valid.eq(0)
        for _ in range(4):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.tx_source.valid) and (yield dut.tx_source.ready):
                tx_out.append((yield dut.tx_source.data))
            if (yield dut.rx_source.valid) and (yield dut.rx_source.ready):
                rx_out.append((yield dut.rx_source.data))
            yield

    run_simulation(dut, [gen(), mon()])

    assert tx_out == [0xAAAA]
    assert rx_out == [0xBBBB, 0xCCCC]
