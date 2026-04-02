#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ptp_discipline import PTPTimeDiscipline


def test_ptp_discipline_requests_coarse_step_on_first_lock():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.local_time.eq(0)
        yield dut.ptp_seconds.eq(1)
        yield dut.ptp_nanoseconds.eq(234)
        yield dut.ptp_locked.eq(1)
        for _ in range(4):
            if (yield dut.discipline_write):
                seen["write"] = 1
                seen["write_time"] = (yield dut.discipline_write_time)
            yield

    run_simulation(dut, gen())
    assert seen.get("write", 0) == 1
    assert seen["write_time"] == 1_000_000_234


def test_ptp_discipline_enters_holdover_and_keeps_last_trim():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)

        yield dut.local_time.eq(0)
        yield dut.ptp_nanoseconds.eq(500_000)
        yield dut.ptp_locked.eq(1)
        yield

        yield dut.local_time.eq(100_100)
        yield dut.ptp_nanoseconds.eq(100_000)
        yield dut.ptp_locked.eq(1)
        for _ in range(3):
            yield
        seen["time_inc_locked"] = (yield dut.discipline_time_inc)

        yield dut.ptp_locked.eq(0)
        for _ in range(3):
            yield
        seen["holdover"] = (yield dut.holdover)
        seen["state"] = (yield dut.state)
        seen["time_inc_holdover"] = (yield dut.discipline_time_inc)
        yield
        seen["time_inc_holdover_stable"] = (yield dut.discipline_time_inc)

    run_simulation(dut, gen())
    assert seen["time_inc_locked"] != (10 << 24)
    assert seen["holdover"] == 1
    assert seen["state"] == dut.STATE_HOLDOVER
    assert seen["time_inc_holdover"] != (10 << 24)
    assert seen["time_inc_holdover_stable"] == seen["time_inc_holdover"]


def test_ptp_discipline_holdover_can_be_disabled_at_runtime():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.holdover_enable.eq(0)
        yield dut.update_cycles_cfg.eq(1)

        yield dut.local_time.eq(0)
        yield dut.ptp_nanoseconds.eq(250_000)
        yield dut.ptp_locked.eq(1)
        yield

        yield dut.local_time.eq(100_050)
        yield dut.ptp_nanoseconds.eq(100_000)
        yield dut.ptp_locked.eq(1)
        for _ in range(3):
            yield
        seen["time_inc_locked"] = (yield dut.discipline_time_inc)

        yield dut.ptp_locked.eq(0)
        for _ in range(3):
            yield
        seen["holdover"] = (yield dut.holdover)
        seen["state"] = (yield dut.state)
        seen["time_inc_after_unlock"] = (yield dut.discipline_time_inc)

    run_simulation(dut, gen())
    assert seen["time_inc_locked"] != (10 << 24)
    assert seen["holdover"] == 0
    assert seen["state"] == dut.STATE_ACQUIRE
    assert seen["time_inc_after_unlock"] == (10 << 24)


def test_ptp_discipline_counts_lock_losses_after_runtime_lock():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(1_000_000)
        yield dut.phase_lock_window_cfg.eq(64)

        yield dut.local_time.eq(0)
        yield dut.ptp_nanoseconds.eq(100_000)
        yield dut.ptp_locked.eq(1)
        yield

        yield dut.local_time.eq(100_000)
        yield dut.ptp_nanoseconds.eq(100_000)
        yield dut.ptp_locked.eq(1)
        for _ in range(3):
            yield
        seen["time_locked"] = (yield dut.locked)

        yield dut.ptp_locked.eq(0)
        for _ in range(3):
            yield
        seen["ptp_lock_losses"] = (yield dut.ptp_lock_losses)
        seen["time_lock_losses"] = (yield dut.time_lock_losses)

    run_simulation(dut, gen())
    assert seen["time_locked"] == 1
    assert seen["ptp_lock_losses"] == 1
    assert seen["time_lock_losses"] == 1


def test_ptp_discipline_runtime_phase_threshold_suppresses_small_adjustments():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {"adjust": 0}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.phase_threshold_cfg.eq(512)

        yield dut.local_time.eq(0)
        yield dut.ptp_nanoseconds.eq(2_000)
        yield dut.ptp_locked.eq(1)
        yield

        yield dut.local_time.eq(1_950)
        yield dut.ptp_nanoseconds.eq(2_000)
        yield dut.ptp_locked.eq(1)
        for _ in range(4):
            if (yield dut.discipline_adjust):
                seen["adjust"] = 1
            yield

    run_simulation(dut, gen())
    assert seen["adjust"] == 0
