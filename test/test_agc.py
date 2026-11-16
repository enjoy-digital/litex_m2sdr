#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from types import SimpleNamespace

from litex.gen import LiteXModule

from litex.soc.interconnect import stream

from litepcie.common import dma_layout

from litex_m2sdr.gateware.ad9361.agc import (
    AGC_DEFAULT_HIGH_THRESHOLD,
    AGCSaturationCount,
)
from litex_m2sdr.gateware.ad9361.core import AD9361RFIC

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


class _AGCHarness(LiteXModule):
    """RX stream of the RFIC core (after its CDC) with the AGC counters of AD9361RFIC.add_agc()."""
    def __init__(self, data_width):
        self.rx_cdc = SimpleNamespace(source=stream.Endpoint(dma_layout(data_width)))
        AD9361RFIC.add_agc(self)


def _run_agc_harness(data_width, words, threshold=1000):
    dut      = _AGCHarness(data_width)
    counters = {
        "rx1_low"  : dut.agc_count_rx1_low,
        "rx1_high" : dut.agc_count_rx1_high,
        "rx2_low"  : dut.agc_count_rx2_low,
        "rx2_high" : dut.agc_count_rx2_high,
    }
    counts = {}

    def gen():
        for counter in counters.values():
            yield counter.control.fields.enable.eq(1)
            yield counter.control.fields.threshold.eq(threshold)
        yield
        yield dut.rx_cdc.source.ready.eq(1)
        for word in words:
            yield dut.rx_cdc.source.valid.eq(1)
            yield dut.rx_cdc.source.data.eq(word)
            yield
        yield dut.rx_cdc.source.valid.eq(0)
        yield
        yield
        for name, counter in counters.items():
            counts[name] = (yield counter.status.fields.count)

    run_simulation(dut, gen())
    return counts


def _agc_word(samples):
    word = 0
    for n, sample in enumerate(samples):
        word |= (sample & 0xffff) << (16*n)
    return word


def test_agc_counters_64b_datapath_observe_each_channel():
    """Verify the AGC counters of the 64-bit datapath: one event per word, per channel."""
    words = [
        _agc_word([   0,     0,    0,     0]),
        _agc_word([1500,     0,    0,     0]), # RX1 I.
        _agc_word([   0, -1500,    0,     0]), # RX1 Q.
        _agc_word([   0,     0, 1500,     0]), # RX2 I.
        _agc_word([1500,  1500,    0, -1500]), # RX1 I/Q and RX2 Q.
    ]
    counts = _run_agc_harness(64, words)
    assert counts == {"rx1_low": 3, "rx1_high": 3, "rx2_low": 2, "rx2_high": 2}


def test_agc_counters_128b_datapath_observe_both_sample_groups():
    """Verify the AGC counters of the 128-bit (oversampling) datapath observe the two sample
    groups of each word: a saturation in the second group alone is counted."""
    words = [
        _agc_word([   0,     0,    0,     0,    0,     0,    0,     0]),
        _agc_word([1500,     0,    0,     0,    0,     0,    0,     0]), # RX1 I, first group.
        _agc_word([   0,     0,    0,     0, 1500,     0,    0,     0]), # RX1 I, second group.
        _agc_word([   0,     0,    0,     0,    0, -1500,    0,     0]), # RX1 Q, second group.
        _agc_word([   0,     0,    0,     0,    0,     0, 1500,     0]), # RX2 I, second group.
        _agc_word([   0,     0,    0,     0,    0,     0,    0, -1500]), # RX2 Q, second group.
        _agc_word([   0,     0, 1500,     0,    0,     0, 1500,     0]), # RX2 I, both groups.
    ]
    counts = _run_agc_harness(128, words)
    assert counts == {"rx1_low": 3, "rx1_high": 3, "rx2_low": 3, "rx2_high": 3}
