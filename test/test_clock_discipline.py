#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import ClockSignal, Signal
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.clock_discipline import MMCMPhaseDiscipline, PTPClock10Discipline


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


def test_mmcm_phase_discipline_external_override_drives_rate():
    dut = MMCMPhaseDiscipline(sys_clk_freq=8, with_csr=False)
    seen = {}

    def gen():
        yield dut.override.eq(1)
        yield dut.override_enable.eq(1)
        yield dut.override_update_cycles.eq(1)
        yield dut.override_rate.eq(0x40000000)
        for _ in range(16):
            yield
        seen["steps"] = (yield dut.step_count)
        seen["inc"] = (yield dut.inc_count)
        seen["dec"] = (yield dut.dec_count)

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


def test_ptp_clock10_discipline_exposes_status_and_mmcm_override_interface():
    reference_pulse = Signal()
    reference_valid = Signal()
    dut = PTPClock10Discipline(
        sys_clk_freq    = 8,
        clk10           = Signal(),
        reference_pulse = reference_pulse,
        reference_valid = reference_valid,
        clk10_freq      = 2,
        with_csr        = True,
    )

    assert hasattr(dut, "mmcm_override")
    assert hasattr(dut, "mmcm_enable")
    assert hasattr(dut, "mmcm_rate")
    assert hasattr(dut, "_status")
    assert hasattr(dut, "_last_error_ns")
    assert hasattr(dut, "_i_gain")
    assert hasattr(dut, "_rate_update_count")
    assert dut.update_cycles_cfg.reset.value == 1
    assert dut.i_gain_cfg.reset.value == 0x00010000
    assert dut.rate_limit_cfg.reset.value == 0x7fffffff


def test_ptp_clock10_discipline_defaults_to_200ns_update_interval():
    reference_pulse = Signal()
    reference_valid = Signal()
    dut = PTPClock10Discipline(
        sys_clk_freq    = 100e6,
        clk10           = Signal(),
        reference_pulse = reference_pulse,
        reference_valid = reference_valid,
        clk10_freq      = 10e6,
        with_csr        = True,
    )

    assert dut.update_cycles_cfg.reset.value == 20
    assert dut.lock_window_ticks_cfg.reset.value == 5000
    assert dut._update_cycles.storage.reset.value == 20
    assert dut._lock_window_ticks.storage.reset.value == 5000


def test_ptp_clock10_discipline_filters_dense_reference_edges():
    reference_pulse = Signal()
    reference_valid = Signal()
    dut = PTPClock10Discipline(
        sys_clk_freq    = 8,
        clk10           = ClockSignal("clk10_mon"),
        reference_pulse = reference_pulse,
        reference_valid = reference_valid,
        clk10_freq      = 2,
        with_csr        = False,
    )
    seen = {}

    def gen():
        yield reference_valid.eq(1)
        yield dut.half_period_ticks_cfg.eq(4)

        for _ in range(6):
            yield reference_pulse.eq(1)
            yield
            yield reference_pulse.eq(0)
            yield
        seen["dense"] = (yield dut.reference_count)

        for _ in range(5):
            yield
        yield reference_pulse.eq(1)
        yield
        yield reference_pulse.eq(0)
        yield
        seen["spaced"] = (yield dut.reference_count)

    run_simulation(dut, gen(), clocks={"sys": 10, "clk10_mon": 50})

    assert seen["dense"] == 0
    assert seen["spaced"] == 1


def test_ptp_clock10_discipline_waits_when_previous_marker_is_too_old():
    reference_pulse = Signal()
    reference_valid = Signal()
    dut = PTPClock10Discipline(
        sys_clk_freq    = 8,
        clk10           = ClockSignal("clk10_mon"),
        reference_pulse = reference_pulse,
        reference_valid = reference_valid,
        clk10_freq      = 1,
        with_csr        = False,
    )
    seen = {}

    def gen():
        yield reference_valid.eq(1)
        yield dut.half_period_ticks_cfg.eq(4)

        while (yield dut.clk10_count) < 1:
            yield

        for _ in range(6):
            yield

        samples_before = (yield dut.sample_count)
        yield reference_pulse.eq(1)
        yield
        yield reference_pulse.eq(0)
        yield

        seen["samples_after_ref"] = (yield dut.sample_count)
        seen["waiting_after_ref"] = (yield dut.waiting_after_ref)

        for _ in range(20):
            if (yield dut.sample_count) > samples_before:
                break
            yield

        seen["samples_after_marker"] = (yield dut.sample_count)
        seen["last_error_ticks"] = (yield dut.last_error_ticks)

    run_simulation(dut, gen(), clocks={"sys": 10, "clk10_mon": 100})

    assert seen["samples_after_ref"] == 0
    assert seen["waiting_after_ref"] == 1
    assert seen["samples_after_marker"] == 1
    assert seen["last_error_ticks"] > 0


def test_ptp_clock10_manual_align_waits_for_reference_edge():
    reference_pulse = Signal()
    reference_valid = Signal()
    dut = PTPClock10Discipline(
        sys_clk_freq    = 8,
        clk10           = ClockSignal("clk10_mon"),
        reference_pulse = reference_pulse,
        reference_valid = reference_valid,
        clk10_freq      = 1,
        with_csr        = False,
    )
    seen = {}

    def pulse_reference():
        yield reference_pulse.eq(1)
        yield
        yield reference_pulse.eq(0)
        yield

    def gen():
        yield dut.enable.eq(1)
        yield reference_valid.eq(1)
        yield dut.half_period_ticks_cfg.eq(2)

        for _ in range(4):
            yield

        yield from pulse_reference()
        seen["initial_align_count"] = (yield dut.align_count)

        yield dut.manual_align.eq(1)
        yield
        yield dut.manual_align.eq(0)
        yield
        seen["after_manual_request"] = (yield dut.align_count)

        for _ in range(4):
            yield

        yield from pulse_reference()
        seen["after_reference"] = (yield dut.align_count)

    run_simulation(dut, gen(), clocks={"sys": 10, "clk10_mon": 100})

    assert seen["initial_align_count"] == 1
    assert seen["after_manual_request"] == 1
    assert seen["after_reference"] == 2
