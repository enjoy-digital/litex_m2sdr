#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ad9361.agc import (
    AGC_DEFAULT_HIGH_THRESHOLD,
    AGCSaturationCount,
)

# AGC Tests ---------------------------------------------------------------------------------------


def test_agc_saturation_counter():
    """Verify threshold counting and explicit clear behavior on the AGC counter."""
    i = Signal(12)
    q = Signal(12)
    dut = AGCSaturationCount(ce=1, iqs=[i, q], inc=1)
    counts = []

    def gen():
        yield dut.control.fields.enable.eq(1)
        yield dut.control.fields.threshold.eq(8)

        # Below threshold.
        for _ in range(4):
            yield i.eq(1)
            yield q.eq(2)
            counts.append((yield dut.status.fields.count))
            yield

        # Above threshold on I.
        for _ in range(5):
            yield i.eq(9)
            yield q.eq(0)
            counts.append((yield dut.status.fields.count))
            yield

        # Clear.
        yield i.eq(0)
        yield q.eq(0)
        yield
        yield dut.control.fields.clear.eq(1)
        counts.append((yield dut.status.fields.count))
        yield
        yield dut.control.fields.clear.eq(0)
        yield
        counts.append((yield dut.status.fields.count))
        yield

    run_simulation(dut, gen())
    assert max(counts[:4]) == 0
    assert counts[8] >= 3
    assert counts[-1] == 0


def test_agc_disable_clears_count():
    """Verify disabling AGC forces the saturation count back to zero."""
    i = Signal(12)
    q = Signal(12)
    dut = AGCSaturationCount(ce=1, iqs=[i, q], inc=1)
    observed = {}

    def gen():
        yield dut.control.fields.enable.eq(1)
        yield dut.control.fields.threshold.eq(2)
        for _ in range(6):
            yield i.eq(5)
            yield q.eq(0)
            yield
        observed["count_enabled"] = (yield dut.status.fields.count)
        yield dut.control.fields.enable.eq(0)
        yield
        yield
        observed["count_disabled"] = (yield dut.status.fields.count)

    run_simulation(dut, gen())
    assert observed["count_enabled"] > 0
    assert observed["count_disabled"] == 0


def test_agc_counts_negative_full_scale():
    """Verify two's-complement minimum samples count as saturated."""
    i = Signal(12)
    q = Signal(12)
    dut = AGCSaturationCount(ce=1, iqs=[i, q], inc=1)
    observed = {}

    def gen():
        yield dut.control.fields.enable.eq(1)
        yield dut.control.fields.threshold.eq(2048)
        yield i.eq(0x800) # -2048 in 12-bit two's complement.
        yield q.eq(0)
        yield
        yield
        observed["count"] = (yield dut.status.fields.count)

    run_simulation(dut, gen())
    assert observed["count"] == 1


def test_agc_counts_one_event_per_beat():
    """Verify simultaneous I/Q saturation increments the event counter once."""
    i = Signal(12)
    q = Signal(12)
    dut = AGCSaturationCount(ce=1, iqs=[i, q], inc=1)
    observed = {}

    def gen():
        yield dut.control.fields.enable.eq(1)
        yield dut.control.fields.threshold.eq(8)
        yield i.eq(9)
        yield q.eq(10)
        yield
        for _ in range(4):
            yield
        observed["count"] = (yield dut.status.fields.count)

    run_simulation(dut, gen())
    assert observed["count"] == 4


def test_agc_threshold_default_is_near_clip():
    """Verify the counter powers up with a useful high-threshold default."""
    i = Signal(12)
    q = Signal(12)
    dut = AGCSaturationCount(ce=1, iqs=[i, q], inc=1)
    observed = {}

    def gen():
        observed["threshold"] = (yield dut.control.fields.threshold)
        yield

    run_simulation(dut, gen())
    assert observed["threshold"] == AGC_DEFAULT_HIGH_THRESHOLD
