#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.pcie import PCIeLinkResetWorkaround

# PCIe Link Reset Workaround Tests ----------------------------------------------------------------

def test_pcie_reset_workaround_stays_high_when_link_is_up():
    """Verify reset output stays deasserted when link-up is already asserted."""
    link_up = Signal(reset=1)
    dut = PCIeLinkResetWorkaround(link_up=link_up, sys_clk_freq=100e6, interval_cycles=4)
    trace = []

    def gen():
        for _ in range(24):
            trace.append((yield dut.rst_n))
            yield

    run_simulation(dut, gen())
    assert all(v == 1 for v in trace)


def test_pcie_reset_workaround_toggles_when_link_is_down():
    """Verify workaround periodically drives reset low/high while link remains down."""
    link_up = Signal(reset=0)
    dut = PCIeLinkResetWorkaround(link_up=link_up, sys_clk_freq=100e6, interval_cycles=4)
    trace = []

    def gen():
        for _ in range(32):
            trace.append((yield dut.rst_n))
            yield

    run_simulation(dut, gen())
    assert 0 in trace
    assert 1 in trace


def test_pcie_reset_workaround_stops_toggling_after_link_up():
    """Verify reset toggling stops once link-up is observed."""
    link_up = Signal(reset=0)
    dut = PCIeLinkResetWorkaround(link_up=link_up, sys_clk_freq=100e6, interval_cycles=4)
    trace = []

    def gen():
        for i in range(40):
            if i == 16:
                yield link_up.eq(1)
            trace.append((yield dut.rst_n))
            yield

    run_simulation(dut, gen())
    assert 0 in trace[:20]
    assert all(v == 1 for v in trace[28:])
