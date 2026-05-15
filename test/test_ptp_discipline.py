#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ptp_discipline import PTPTimeDiscipline


def _wait_for_servo_sample(dut, expected_ptp_time, expected_local_time, timeout=128):
    for _ in range(timeout):
        yield
        if (
            ((yield dut.last_ptp_time) == expected_ptp_time) and
            ((yield dut.last_local_time) == expected_local_time)
        ):
            return
    raise AssertionError("PTP discipline sample did not complete")


def _apply_servo_sample(dut, local_time, ptp_nanoseconds, ptp_locked=1):
    coarse_steps = yield dut.coarse_steps
    rate_updates = yield dut.rate_updates
    expected_ptp_time = ptp_nanoseconds

    yield dut.local_time.eq(local_time)
    yield dut.ptp_seconds.eq(0)
    yield dut.ptp_nanoseconds.eq(ptp_nanoseconds)
    yield dut.ptp_locked.eq(ptp_locked)

    for _ in range(128):
        yield
        changed = ((yield dut.coarse_steps) != coarse_steps) or ((yield dut.rate_updates) != rate_updates)
        sampled = (
            ((yield dut.last_ptp_time) == expected_ptp_time) and
            ((yield dut.last_local_time) == local_time)
        )
        if changed and sampled:
            return
    raise AssertionError("PTP discipline sample did not complete")


def _apply_rejected_servo_sample(dut, local_time, ptp_nanoseconds, ptp_locked=1):
    misses = yield dut.time_lock_misses

    yield dut.local_time.eq(local_time)
    yield dut.ptp_seconds.eq(0)
    yield dut.ptp_nanoseconds.eq(ptp_nanoseconds)
    yield dut.ptp_locked.eq(ptp_locked)

    for _ in range(128):
        yield
        if (yield dut.time_lock_misses) == misses + 1:
            return
    raise AssertionError("PTP discipline sample was not rejected")


def test_ptp_discipline_default_disables_runtime_coarse_realignment():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    assert dut.phase_lock_window_cfg.reset.value == 4_096
    assert dut.unlock_misses_cfg.reset.value == 64
    assert dut.coarse_confirm_cfg.reset.value == 0

    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(100_000)
        yield dut.phase_lock_window_cfg.eq(500)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["coarse_before"] = (yield dut.coarse_steps)
        seen["last_error_before"] = (yield dut.last_error)

        for _ in range(4):
            yield from _apply_rejected_servo_sample(
                dut,
                local_time=400_000,
                ptp_nanoseconds=100_000,
            )

        seen["locked_after_outliers"] = (yield dut.locked)
        seen["coarse_after"] = (yield dut.coarse_steps)
        seen["last_error_after"] = (yield dut.last_error)
        seen["miss_count_after"] = (yield dut.time_lock_miss_count)

    run_simulation(dut, gen())
    assert seen["locked_after_outliers"] == 1
    assert seen["coarse_after"] == seen["coarse_before"]
    assert seen["last_error_after"] == seen["last_error_before"]
    assert seen["miss_count_after"] == 0


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
        for _ in range(32):
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
        for _ in range(32):
            yield
        seen["time_inc_locked"] = (yield dut.discipline_time_inc)

        yield dut.ptp_locked.eq(0)
        for _ in range(32):
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
        for _ in range(32):
            yield
        seen["time_inc_locked"] = (yield dut.discipline_time_inc)

        yield dut.ptp_locked.eq(0)
        for _ in range(32):
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
        for _ in range(32):
            yield
        seen["time_locked"] = (yield dut.locked)

        yield dut.ptp_locked.eq(0)
        for _ in range(32):
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
        for _ in range(32):
            if (yield dut.discipline_adjust):
                seen["adjust"] = 1
            yield

    run_simulation(dut, gen())
    assert seen["adjust"] == 0


def test_ptp_discipline_tolerates_isolated_out_of_window_sample():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(1_000_000)
        yield dut.phase_lock_window_cfg.eq(500)
        yield dut.unlock_misses_cfg.eq(3)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["locked_before_miss"] = (yield dut.locked)

        yield from _apply_servo_sample(dut, local_time=102_000, ptp_nanoseconds=100_000)
        seen["locked_after_miss"] = (yield dut.locked)
        seen["state_after_miss"] = (yield dut.state)
        seen["misses_after_miss"] = (yield dut.time_lock_misses)
        seen["miss_count_after_miss"] = (yield dut.time_lock_miss_count)
        seen["losses_after_miss"] = (yield dut.time_lock_losses)

        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["miss_count_after_relock_sample"] = (yield dut.time_lock_miss_count)

    run_simulation(dut, gen())
    assert seen["locked_before_miss"] == 1
    assert seen["locked_after_miss"] == 1
    assert seen["state_after_miss"] == dut.STATE_LOCKED
    assert seen["misses_after_miss"] == 1
    assert seen["miss_count_after_miss"] == 1
    assert seen["losses_after_miss"] == 0
    assert seen["miss_count_after_relock_sample"] == 0


def test_ptp_discipline_runtime_coarse_realign_keeps_lock_state():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(100_000)
        yield dut.phase_lock_window_cfg.eq(500)
        yield dut.coarse_confirm_cfg.eq(1)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["locked_before_coarse"] = (yield dut.locked)
        seen["losses_before_coarse"] = (yield dut.time_lock_losses)
        seen["coarse_before"] = (yield dut.coarse_steps)

        yield from _apply_servo_sample(dut, local_time=300_000, ptp_nanoseconds=100_000)
        seen["locked_after_coarse"] = (yield dut.locked)
        seen["state_after_coarse"] = (yield dut.state)
        seen["losses_after_coarse"] = (yield dut.time_lock_losses)
        seen["coarse_after"] = (yield dut.coarse_steps)

    run_simulation(dut, gen())
    assert seen["locked_before_coarse"] == 1
    assert seen["locked_after_coarse"] == 1
    assert seen["state_after_coarse"] == dut.STATE_LOCKED
    assert seen["losses_after_coarse"] == seen["losses_before_coarse"]
    assert seen["coarse_after"] == seen["coarse_before"] + 1


def test_ptp_discipline_rejects_isolated_runtime_coarse_outlier():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(100_000)
        yield dut.phase_lock_window_cfg.eq(500)
        yield dut.unlock_misses_cfg.eq(3)
        yield dut.coarse_confirm_cfg.eq(3)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["locked_before_outlier"] = (yield dut.locked)
        seen["coarse_before"] = (yield dut.coarse_steps)
        seen["last_error_before"] = (yield dut.last_error)
        seen["last_ptp_time_before"] = (yield dut.last_ptp_time)

        yield from _apply_rejected_servo_sample(
            dut,
            local_time=400_000,
            ptp_nanoseconds=100_000,
        )
        seen["locked_after_outlier"] = (yield dut.locked)
        seen["coarse_after_outlier"] = (yield dut.coarse_steps)
        seen["last_error_after_outlier"] = (yield dut.last_error)
        seen["last_ptp_time_after_outlier"] = (yield dut.last_ptp_time)
        seen["misses_after_outlier"] = (yield dut.time_lock_misses)
        seen["miss_count_after_outlier"] = (yield dut.time_lock_miss_count)

        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["miss_count_after_good_sample"] = (yield dut.time_lock_miss_count)

    run_simulation(dut, gen())
    assert seen["locked_before_outlier"] == 1
    assert seen["locked_after_outlier"] == 1
    assert seen["coarse_after_outlier"] == seen["coarse_before"]
    assert seen["last_error_after_outlier"] == seen["last_error_before"]
    assert seen["last_ptp_time_after_outlier"] == seen["last_ptp_time_before"]
    assert seen["misses_after_outlier"] == 1
    assert seen["miss_count_after_outlier"] == 1
    assert seen["miss_count_after_good_sample"] == 0


def test_ptp_discipline_applies_confirmed_runtime_coarse_realign():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(100_000)
        yield dut.phase_lock_window_cfg.eq(500)
        yield dut.unlock_misses_cfg.eq(2)
        yield dut.coarse_confirm_cfg.eq(2)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["coarse_before"] = (yield dut.coarse_steps)

        yield from _apply_rejected_servo_sample(
            dut,
            local_time=400_000,
            ptp_nanoseconds=100_000,
        )
        yield from _apply_servo_sample(
            dut,
            local_time=400_000,
            ptp_nanoseconds=100_000,
        )
        seen["locked_after_coarse"] = (yield dut.locked)
        seen["state_after_coarse"] = (yield dut.state)
        seen["coarse_after"] = (yield dut.coarse_steps)
        seen["miss_count_after_coarse"] = (yield dut.time_lock_miss_count)

    run_simulation(dut, gen())
    assert seen["locked_after_coarse"] == 1
    assert seen["state_after_coarse"] == dut.STATE_LOCKED
    assert seen["coarse_after"] == seen["coarse_before"] + 1
    assert seen["miss_count_after_coarse"] == 0


def test_ptp_discipline_never_applies_runtime_one_second_outlier():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(100_000)
        yield dut.phase_lock_window_cfg.eq(500)
        yield dut.unlock_misses_cfg.eq(2)
        yield dut.coarse_confirm_cfg.eq(2)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["coarse_before"] = (yield dut.coarse_steps)
        seen["last_error_before"] = (yield dut.last_error)

        for _ in range(4):
            yield from _apply_rejected_servo_sample(
                dut,
                local_time=1_000_100_000,
                ptp_nanoseconds=100_000,
            )

        seen["locked_after_outliers"] = (yield dut.locked)
        seen["state_after_outliers"] = (yield dut.state)
        seen["coarse_after"] = (yield dut.coarse_steps)
        seen["last_error_after"] = (yield dut.last_error)
        seen["miss_count_after"] = (yield dut.time_lock_miss_count)

    run_simulation(dut, gen())
    assert seen["locked_after_outliers"] == 1
    assert seen["state_after_outliers"] == dut.STATE_LOCKED
    assert seen["coarse_after"] == seen["coarse_before"]
    assert seen["last_error_after"] == seen["last_error_before"]
    assert seen["miss_count_after"] == 0


def test_ptp_discipline_retries_ptp_second_rollover_sample():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(1_000_000)
        yield dut.phase_lock_window_cfg.eq(500)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        seen["coarse_before"] = (yield dut.coarse_steps)

        yield dut.local_time.eq(11_000_000_010)
        yield dut.ptp_seconds.eq(10)
        yield dut.ptp_nanoseconds.eq(10)
        yield dut.ptp_locked.eq(1)
        yield

        yield dut.local_time.eq(11_000_000_010)
        yield dut.ptp_seconds.eq(11)
        yield dut.ptp_nanoseconds.eq(10)
        yield
        yield

        yield from _wait_for_servo_sample(
            dut,
            expected_ptp_time=11_000_000_010,
            expected_local_time=11_000_000_010,
        )
        seen["last_error"] = (yield dut.last_error)
        seen["last_ptp_time"] = (yield dut.last_ptp_time)
        seen["last_local_time"] = (yield dut.last_local_time)
        seen["coarse_after"] = (yield dut.coarse_steps)

    run_simulation(dut, gen())
    assert seen["last_error"] == 0
    assert seen["last_ptp_time"] == 11_000_000_010
    assert seen["last_local_time"] == 11_000_000_010
    assert seen["coarse_after"] == seen["coarse_before"]


def test_ptp_discipline_drops_lock_after_consecutive_out_of_window_samples():
    dut = PTPTimeDiscipline(sys_clk_freq=64, nominal_time_inc=(10 << 24), with_csr=False)
    seen = {}

    def gen():
        yield dut.enable.eq(1)
        yield dut.update_cycles_cfg.eq(1)
        yield dut.coarse_threshold_cfg.eq(1_000_000)
        yield dut.phase_lock_window_cfg.eq(500)
        yield dut.unlock_misses_cfg.eq(2)

        yield from _apply_servo_sample(dut, local_time=0, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=100_000, ptp_nanoseconds=100_000)
        yield from _apply_servo_sample(dut, local_time=102_000, ptp_nanoseconds=100_000)
        seen["locked_after_first_miss"] = (yield dut.locked)
        seen["miss_count_after_first_miss"] = (yield dut.time_lock_miss_count)

        yield from _apply_servo_sample(dut, local_time=102_000, ptp_nanoseconds=100_000)
        seen["locked_after_second_miss"] = (yield dut.locked)
        seen["state_after_second_miss"] = (yield dut.state)
        seen["miss_count_after_second_miss"] = (yield dut.time_lock_miss_count)
        seen["misses_after_second_miss"] = (yield dut.time_lock_misses)
        seen["losses_after_second_miss"] = (yield dut.time_lock_losses)

    run_simulation(dut, gen())
    assert seen["locked_after_first_miss"] == 1
    assert seen["miss_count_after_first_miss"] == 1
    assert seen["locked_after_second_miss"] == 0
    assert seen["state_after_second_miss"] == dut.STATE_ACQUIRE
    assert seen["miss_count_after_second_miss"] == 0
    assert seen["misses_after_second_miss"] == 1
    assert seen["losses_after_second_miss"] == 1
