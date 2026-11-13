#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.time import TimeGenerator, TimeNsToPS

# Time Core Tests ---------------------------------------------------------------------------------


def test_time_generator_basic_increment_and_write():
    """Validate free-running increment and software write path of TimeGenerator."""
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


def test_time_generator_time_domain_is_reset_less():
    """Validate the SI5351-driven time domain has no generated async reset path."""
    dut = TimeGenerator(clk=ClockSignal("time"), clk_freq=100e6, with_csr=False)

    assert dut.cd_time.rst is None


def test_time_generator_adjust_and_disable_reset():
    """Validate time adjustment effect and value drop after disable/reset path."""
    dut = TimeGenerator(clk=ClockSignal("time"), clk_freq=100e6, init=42, with_csr=False)
    samples = {}

    def gen():
        yield dut.enable.eq(1)
        for _ in range(8):
            yield
        yield dut.time_adjustment.eq(1000)
        for _ in range(3):
            yield
        samples["with_adjust"] = (yield dut.time)
        yield dut.enable.eq(0)
        yield dut.time_adjustment.eq(0)
        for _ in range(2):
            yield
        samples["after_disable"] = (yield dut.time)

    run_simulation(dut, gen(), clocks={"sys": 10, "time": 10})
    assert samples["with_adjust"] >= 1000
    assert samples["after_disable"] < samples["with_adjust"]


def test_time_ns_to_ps_conversion():
    """Validate TimeNsToPS converts ns remainder into picosecond fraction correctly."""
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


def test_time_generator_reenable_restarts_from_init():
    """Validate disabling then re-enabling restarts TimeGenerator from init baseline."""
    dut = TimeGenerator(clk=ClockSignal("time"), clk_freq=100e6, init=77, with_csr=False)
    samples = {}

    def gen():
        yield dut.enable.eq(1)
        for _ in range(8):
            yield
        samples["running"] = (yield dut.time)
        yield dut.enable.eq(0)
        for _ in range(3):
            yield
        samples["disabled"] = (yield dut.time)
        yield dut.enable.eq(1)
        for _ in range(2):
            yield
        samples["reenabled"] = (yield dut.time)

    run_simulation(dut, gen(), clocks={"sys": 10, "time": 10})
    assert samples["running"] > 77
    assert samples["disabled"] <= samples["running"]
    assert samples["reenabled"] >= 77


def test_time_generator_external_discipline_write_and_adjust():
    """Validate external discipline can steer time without disturbing the CSR ABI."""
    dut = TimeGenerator(clk=ClockSignal("time"), clk_freq=100e6, with_csr=False)
    samples = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.discipline_enable.eq(1)
        yield dut.discipline_time_inc.eq(20 << 24)
        for _ in range(6):
            yield
        samples["faster"] = (yield dut.time)

        yield dut.discipline_write_time.eq(1_000_000)
        yield dut.discipline_write.eq(1)
        yield
        yield dut.discipline_write.eq(0)
        for _ in range(2):
            yield
        samples["written"] = (yield dut.time)

        yield dut.discipline_adjust_sign.eq(1)
        yield dut.discipline_adjustment.eq(250)
        yield dut.discipline_adjust.eq(1)
        yield
        yield dut.discipline_adjust.eq(0)
        for _ in range(2):
            yield
        samples["adjusted"] = (yield dut.time)

    run_simulation(dut, gen(), clocks={"sys": 10, "time": 10})
    assert samples["faster"] > 60
    assert samples["written"] >= 1_000_000
    assert samples["adjusted"] <= samples["written"] - 200


def _run_time_change_cdc(sys_period, time_period, writes=100, spacing=53):
    # Even periods only: the simulator rounds the half period of a clock down.
    assert (sys_period % 2 == 0) and (time_period % 2 == 0)
    dut = TimeGenerator(clk=ClockSignal("time"), clk_freq=100e6, with_csr=False)
    dut.add_cdc()
    write  = dut.write
    counts = {"sent": 0, "seen": 0, "consecutive": 0}

    def gen_time():
        yield dut.enable.eq(1)
        for i in range(writes*spacing + 64):
            strobe = int((i % spacing == 20) and (counts["sent"] < writes))
            yield write.eq(strobe)
            yield dut.write_time.eq(1000*i)
            counts["sent"] += strobe
            yield

    def gen_sys():
        previous = 0
        for _ in range((writes*spacing + 64)*time_period//sys_period + 64):
            yield
            change = (yield dut.time_change)
            counts["seen"]        += change
            counts["consecutive"] += change & previous
            previous = change

    run_simulation(dut, {"time": gen_time(), "sys": gen_sys()},
        clocks={"sys": sys_period, "time": time_period})
    return counts


def test_time_generator_cdc_time_change_not_lost_with_slower_sys_clock():
    """Validate each time change reaches sys when the crossing drops words (sys slower)."""
    counts = _run_time_change_cdc(sys_period=22, time_period=20)
    assert counts["sent"] == 100
    assert counts["seen"] == counts["sent"]
    assert counts["consecutive"] == 0


def test_time_generator_cdc_time_change_is_one_cycle_with_faster_sys_clock():
    """Validate a time change is a one-cycle pulse in sys when the crossing idles (sys faster)."""
    counts = _run_time_change_cdc(sys_period=16, time_period=20)
    assert counts["sent"] == 100
    assert counts["seen"] == counts["sent"]
    assert counts["consecutive"] == 0


def test_time_generator_cdc_time_change_with_equal_clocks():
    """Validate one sys pulse per time change with equal sys and time clock periods."""
    counts = _run_time_change_cdc(sys_period=20, time_period=20)
    assert counts["sent"] == 100
    assert counts["seen"] == counts["sent"]
    assert counts["consecutive"] == 0
