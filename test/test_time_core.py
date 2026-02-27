#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.time import TimeGenerator, TimeNsToPS


def test_time_generator_basic_increment_and_write():
    dut = TimeGenerator(clk=ClockSignal("time"), clk_freq=100e6, with_csr=False)
    samples = {}

    def gen():
        yield dut.enable.eq(1)
        for _ in range(16):
            yield
        samples["t1"] = (yield dut.time)

        yield dut.write_time.eq(123456789)
        yield dut.write.eq(1)
        yield
        yield dut.write.eq(0)
        for _ in range(4):
            yield
        samples["t2"] = (yield dut.time)

    run_simulation(dut, gen(), clocks={"sys": 10, "time": 10})
    assert samples["t1"] > 0
    assert samples["t2"] >= 123456789


def test_time_ns_to_ps_conversion():
    time_ns = Signal(64)
    time_s = Signal(32)
    time_ps = Signal(64)
    dut = TimeNsToPS(time_ns=time_ns, time_s=time_s, time_ps=time_ps)
    out = {}

    def gen():
        # 12.345678901s with seconds field at 12 should yield 345678901000 ps.
        yield time_ns.eq(12_345_678_901)
        yield time_s.eq(12)
        for _ in range(6):
            yield
        out["ps"] = (yield time_ps)

    run_simulation(dut, gen())
    assert out["ps"] == 345_678_901_000
