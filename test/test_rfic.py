#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.rfic import RFICDataFramer, RFICDataPacketizer


def test_rfic_data_framer_marks_last_on_configured_period():
    """Verify RFICDataFramer asserts last every configured data_words beats."""
    dut = RFICDataFramer(data_width=32, data_words=3)
    captured = []

    def gen():
        yield dut.source.ready.eq(1)
        for i in range(7):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(0x100 + i)
            yield
            yield dut.sink.valid.eq(0)
            yield
        yield dut.sink.valid.eq(0)
        for _ in range(6):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])

    assert [w for w, _ in captured] == [0x100 + i for i in range(7)]
    assert [l for _, l in captured] == [0, 0, 1, 0, 0, 1, 0]


def test_rfic_data_framer_preserves_count_across_idle_gap():
    """Verify framing position continues correctly across an idle gap in the input stream."""
    dut = RFICDataFramer(data_width=32, data_words=4)
    captured = []

    def gen():
        for i in range(3):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(i)
            yield
            yield dut.sink.valid.eq(0)
            yield

        for _ in range(3):
            yield

        for i in range(3, 8):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(i)
            yield
            yield dut.sink.valid.eq(0)
            yield

        yield dut.sink.valid.eq(0)
        for _ in range(6):
            yield

    @passive
    def ready_drive():
        while True:
            yield dut.source.ready.eq(1)
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), ready_drive(), mon()])

    assert [w for w, _ in captured] == list(range(8))
    assert [l for _, l in captured] == [0, 0, 0, 1, 0, 0, 0, 1]


def test_rfic_data_packetizer_constant_data_words_under_stalls():
    """Verify packetizer preserves data_words metadata and framing under output stalls."""
    dut = RFICDataPacketizer(data_width=32, data_words=5)
    captured = []

    def gen():
        for i in range(10):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(0x200 + i)
            while not (yield dut.sink.ready):
                yield
            yield
        yield dut.sink.valid.eq(0)
        for _ in range(16):
            yield

    @passive
    def mon():
        cycle = 0
        while True:
            yield dut.source.ready.eq(0 if cycle in {3, 9, 10} else 1)
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last), (yield dut.source.data_words)))
            cycle += 1
            yield

    run_simulation(dut, [gen(), mon()])

    assert [w for w, _, _ in captured] == [0x200 + i for i in range(10)]
    assert [l for _, l, _ in captured] == [0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    assert all(dw == 5 for _, _, dw in captured)
