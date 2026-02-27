#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.pps import PPSGenerator


def test_pps_generator_pulse_and_count():
    time = Signal(64)
    dut = PPSGenerator(clk_freq=100e6, time=time, offset=0, reset=0)
    pulses = []
    counts = []

    def gen():
        # Simulate time in ns jumping each cycle.
        value = 1
        for _ in range(80):
            value += 50_000_000
            yield time.eq(value)
            pulses.append((yield dut.pps_pulse))
            counts.append((yield dut.count))
            yield

    run_simulation(dut, gen())

    assert any(pulses)
    assert counts[-1] >= 1


def test_pps_generator_stays_idle_when_time_zero():
    time = Signal(64)
    dut = PPSGenerator(clk_freq=100e6, time=time, offset=0, reset=0)
    pulses = []
    counts = []

    def gen():
        for _ in range(32):
            yield time.eq(0)
            pulses.append((yield dut.pps_pulse))
            counts.append((yield dut.count))
            yield

    run_simulation(dut, gen())
    assert not any(pulses)
    assert counts[-1] == 0
