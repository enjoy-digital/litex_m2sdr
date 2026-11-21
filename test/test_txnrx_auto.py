#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ad9361.core import AD9361TXNRXAuto

# AD9361 TXNRX Auto Tests --------------------------------------------------------------------------

def _run(events, cycles=400, hold=20, activity_cycles=8):
    """events: {cycle: (lead, valid, data)} applied from that cycle on. Returns txnrx per cycle."""
    dut = AD9361TXNRXAuto(activity_cycles=activity_cycles)
    trace = []

    def gen():
        yield dut.hold_cycles.eq(hold)
        for cycle in range(cycles):
            if cycle in events:
                lead, valid, data = events[cycle]
                yield dut.lead.eq(lead)
                yield dut.tx_valid.eq(valid)
                yield dut.tx_data.eq(data)
            trace.append((yield dut.txnrx))
            yield

    run_simulation(dut, gen(), clocks={"sys": 10, "rfic": 10})
    return trace


def test_txnrx_auto_idle_zeros_keep_tx_off():
    """Zero samples (cleared DMA buffers between bursts) never turn TX on."""
    trace = _run({0: (0, 1, 0)})
    assert not any(trace)


def test_txnrx_auto_lead_then_data_then_off():
    """TX on from the lead, through the samples, then off hold_cycles after the last one."""
    trace = _run({
        50:  (1, 0, 0),      # Lead: a timed burst airs soon.
        60:  (0, 1, 0x123),  # Burst samples reach the PHY.
        200: (0, 1, 0),      # Burst over: zero buffers.
    })
    assert trace[49] == 0
    assert all(trace[52:200])          # On from the lead, gap lead -> samples bridged by hold.
    off = trace.index(0, 200)
    assert 200 + 20 <= off <= 200 + 20 + 8 + 8, off # hold + activity stretch + sync stages.
    assert not any(trace[off:])


def test_txnrx_auto_lead_without_data_times_out():
    """A lead with no samples after it (dropped late burst) releases after hold_cycles."""
    trace = _run({50: (1, 0, 0), 55: (0, 0, 0)})
    assert all(trace[52:70])
    assert not any(trace[80:])


def test_txnrx_auto_short_zero_run_inside_burst_stays_on():
    """Zero samples shorter than the hold inside a burst keep TX on."""
    trace = _run({
        50:  (0, 1, 0x1),
        100: (0, 1, 0),      # 10 zero samples.
        110: (0, 1, 0x1),
        200: (0, 0, 0),
    })
    assert all(trace[56:200])
