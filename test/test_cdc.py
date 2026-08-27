#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.cdc import ValueStrobeCDC

# ValueStrobeCDC Tests ------------------------------------------------------------------------------


def test_value_strobe_cdc_transfer():
    """A strobed value arrives intact, with strobe_o pulsing once per transfer."""
    dut = ValueStrobeCDC(64, cd_from="sys", cd_to="dst")
    received = []

    def gen():
        for value in [0x0123456789ABCDEF, 0xFFFFFFFFFFFFFFFF, 0]:
            yield dut.value.eq(value)
            yield dut.strobe.eq(1)
            yield
            yield dut.strobe.eq(0)
            for _ in range(16):
                yield

    def mon():
        for _ in range(80):
            if (yield dut.strobe_o):
                received.append((yield dut.value_o))
            yield

    run_simulation(dut, {"sys": gen(), "dst": mon()}, clocks={"sys": 10, "dst": 7})
    assert received == [0x0123456789ABCDEF, 0xFFFFFFFFFFFFFFFF, 0]


def test_value_strobe_cdc_multi_field():
    """Multiple fields cross together and stay consistent."""
    dut = ValueStrobeCDC([("sign", 1), ("value", 64)], cd_from="sys", cd_to="dst")
    received = []

    def gen():
        for sign, value in [(1, 1000), (0, 2000)]:
            yield dut.sign.eq(sign)
            yield dut.value.eq(value)
            yield dut.strobe.eq(1)
            yield
            yield dut.strobe.eq(0)
            for _ in range(16):
                yield

    def mon():
        for _ in range(60):
            if (yield dut.strobe_o):
                received.append(((yield dut.sign_o), (yield dut.value_o)))
            yield

    run_simulation(dut, {"sys": gen(), "dst": mon()}, clocks={"sys": 10, "dst": 7})
    assert received == [(1, 1000), (0, 2000)]


def test_value_strobe_cdc_on_change():
    """With on_change=True, a transfer fires per input change, not per cycle."""
    dut = ValueStrobeCDC(32, cd_from="sys", cd_to="dst", on_change=True)
    received = []

    def gen():
        yield dut.value.eq(111)
        for _ in range(16):
            yield
        yield dut.value.eq(222)
        for _ in range(16):
            yield

    def mon():
        for _ in range(48):
            if (yield dut.strobe_o):
                received.append((yield dut.value_o))
            yield

    run_simulation(dut, {"sys": gen(), "dst": mon()}, clocks={"sys": 10, "dst": 7})
    assert received == [111, 222]


# AsyncFIFORegistered (OSERDES TX group hand-off) --------------------------------------------------


def test_async_fifo_registered_group_handoff():
    """The OSERDES TX group FIFO transfers whole words, in order, with a 4x-fast write clock
    writing one group per 4 write cycles and a continuous reader - the phy.py rfic -> CLKDIV
    hand-off pattern. Groups must never tear or reorder regardless of the domains' phase."""
    from litex_m2sdr.gateware.ad9361.cdc import AsyncFIFORegistered

    # In hardware both clocks derive from DATA_CLK at an exact 4:1 ratio, so the reader is never
    # slower than the group rate; sweep matched and faster-reader periods to vary the phase.
    for read_period in (5, 7, 8):
        dut = AsyncFIFORegistered(width=48, depth=8, register_storage=True,
                                  registered_write=True)
        got = []

        def wr(dut=dut):
            yield dut.we.eq(0)
            for _ in range(4):
                yield
            for n in range(1, 201):
                # One write per 4 write-domain cycles (the tx_count group strobe).
                yield dut.din.eq(n)
                yield dut.we.eq(1)
                yield
                yield dut.we.eq(0)
                for _ in range(3):
                    yield
            for _ in range(40):
                yield

        def rd(dut=dut, got=got):
            yield dut.re.eq(1)
            for _ in range(280):
                if (yield dut.readable):
                    got.append((yield dut.dout))
                yield

        run_simulation(dut, {"write": wr(), "read": rd()},
                       clocks={"write": 2, "read": read_period})
        assert len(got) > 100, (read_period, len(got))
        for a, b in zip(got, got[1:]):
            assert b == a + 1, (read_period, a, b)
