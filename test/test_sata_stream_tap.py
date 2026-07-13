#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import passive, run_simulation

from litex_m2sdr.gateware.sata import SATAStreamTap


def _layout():
    return [("data", 32)]


def test_sata_stream_tap_disabled_keeps_primary_path_independent():
    dut = SATAStreamTap(_layout())
    primary = []
    tapped = []

    @passive
    def monitor():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                primary.append((yield dut.source.data))
            if (yield dut.tap.valid) and (yield dut.tap.ready):
                tapped.append((yield dut.tap.data))
            yield

    def generator():
        yield dut.enable.eq(0)
        yield dut.source.ready.eq(1)
        yield dut.tap.ready.eq(0)
        yield dut.sink.valid.eq(1)
        yield dut.sink.data.eq(0x12345678)
        yield
        yield dut.sink.valid.eq(0)
        for _ in range(6):
            yield

    run_simulation(dut, [generator(), monitor()])
    assert primary == [0x12345678]
    assert tapped == []


def test_sata_stream_tap_advances_only_when_both_consumers_accept():
    dut = SATAStreamTap(_layout())
    primary = []
    tapped = []

    @passive
    def monitor():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                primary.append((yield dut.source.data))
            if (yield dut.tap.valid) and (yield dut.tap.ready):
                tapped.append((yield dut.tap.data))
            yield

    def generator():
        yield dut.enable.eq(1)
        yield dut.source.ready.eq(1)
        yield dut.tap.ready.eq(1)
        for value in range(12):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(0x1000 + value)
            while not (yield dut.sink.ready):
                yield
            yield
        yield dut.sink.valid.eq(0)
        for _ in range(10):
            yield

    run_simulation(dut, [generator(), monitor()])
    expected = [0x1000 + value for value in range(12)]
    assert primary == expected
    assert tapped == expected
