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
