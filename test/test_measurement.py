#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex_m2sdr.gateware.measurement import ClkMeasurement, MultiClkMeasurement


def test_clk_measurement_exposes_expected_interfaces():
    """Verify ClkMeasurement instantiates its counter-domain signals and optional CSRs."""
    dut = ClkMeasurement(clk=ClockSignal("meas"), increment=3, with_csr=True)

    assert hasattr(dut, "latch")
    assert hasattr(dut, "value")
    assert hasattr(dut, "cd_counter")
    assert hasattr(dut, "_latch")
    assert hasattr(dut, "_value")


def test_clk_measurement_without_csr_skips_csr_registers():
    """Verify ClkMeasurement can be used without generating software-visible CSR state."""
    dut = ClkMeasurement(clk=ClockSignal("meas"), with_csr=False)

    assert hasattr(dut, "latch")
    assert hasattr(dut, "value")
    assert not hasattr(dut, "_latch")
    assert not hasattr(dut, "_value")


def test_multi_clk_measurement_registers_each_named_clock():
    """Verify MultiClkMeasurement creates one ClkMeasurement per requested clock."""
    dut = MultiClkMeasurement(
        clks={
            "fast_clk": ClockSignal("fast"),
            "slow_clk": ClockSignal("slow"),
        },
        with_csr=False,
        with_latch_all=False,
    )

    assert set(dut.clk_modules.keys()) == {"fast_clk", "slow_clk"}
    assert isinstance(dut.clk_modules["fast_clk"], ClkMeasurement)
    assert isinstance(dut.clk_modules["slow_clk"], ClkMeasurement)
    assert hasattr(dut, "fast_clk")
    assert hasattr(dut, "slow_clk")
    assert not hasattr(dut, "latch_all")


def test_multi_clk_measurement_optionally_exposes_shared_latch():
    """Verify latch_all is only created when requested with CSR support."""
    dut = MultiClkMeasurement(
        clks={"sys_clk": ClockSignal("sys")},
        with_csr=True,
        with_latch_all=True,
    )

    assert hasattr(dut, "latch_all")
    assert hasattr(dut.clk_modules["sys_clk"], "_latch")
    assert hasattr(dut.clk_modules["sys_clk"], "_value")
