#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.clock_discipline import MMCMPhaseDiscipline


def test_mmcm_phase_discipline_counts_manual_inc_and_dec_steps():
    dut = MMCMPhaseDiscipline(sys_clk_freq=8, with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.manual_inc.eq(1)
        yield
        yield dut.manual_inc.eq(0)
        for _ in range(4):
            yield

        yield dut.manual_dec.eq(1)
        yield
        yield dut.manual_dec.eq(0)
        for _ in range(4):
            yield

        seen["steps"] = (yield dut.step_count)
        seen["inc"] = (yield dut.inc_count)
        seen["dec"] = (yield dut.dec_count)
        seen["last_direction"] = (yield dut.last_direction)

    @passive
    def psdone_responder():
        while True:
            if (yield dut.psen):
                yield
                yield dut.psdone.eq(1)
                yield
                yield dut.psdone.eq(0)
            yield

    run_simulation(dut, [gen(), psdone_responder()])

    assert seen["steps"] == 2
    assert seen["inc"] == 1
    assert seen["dec"] == 1
    assert seen["last_direction"] == 0


def test_mmcm_phase_discipline_generates_positive_auto_steps():
    dut = MMCMPhaseDiscipline(sys_clk_freq=8, with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.rate_cfg.eq(0x40000000)
        for _ in range(16):
            yield
        seen["steps"] = (yield dut.step_count)
        seen["inc"] = (yield dut.inc_count)
        seen["dec"] = (yield dut.dec_count)
        seen["dropped"] = (yield dut.dropped_count)

    @passive
    def psdone_responder():
        while True:
            if (yield dut.psen):
                yield
                yield dut.psdone.eq(1)
                yield
                yield dut.psdone.eq(0)
            yield

    run_simulation(dut, [gen(), psdone_responder()])

    assert seen["steps"] >= 3
    assert seen["inc"] == seen["steps"]
    assert seen["dec"] == 0
    assert seen["dropped"] == 0


def test_mmcm_phase_discipline_counts_dropped_requests_while_busy():
    dut = MMCMPhaseDiscipline(sys_clk_freq=8, with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.rate_cfg.eq(0x70000000)
        for _ in range(12):
            yield
        seen["busy"] = (yield dut.busy)
        seen["steps"] = (yield dut.step_count)
        seen["dropped"] = (yield dut.dropped_count)

    run_simulation(dut, gen())

    assert seen["busy"] == 1
    assert seen["steps"] == 0
    assert seen["dropped"] > 0
