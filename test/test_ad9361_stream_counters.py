#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex_m2sdr.gateware.ad9361.core import AD9361StreamCounters

CLOCKS = {"sys": 10, "rfic": 8}


def _run(dut, gen, settle=40):
    result = {}

    def bench():
        yield from gen()
        for _ in range(settle):
            yield
        result["rx_events"] = (yield dut.rx_overflow_events)
        result["rx_words"]  = (yield dut.rx_overflow_words)
        result["tx_events"] = (yield dut.tx_underflow_events)

    run_simulation(dut, {"rfic": bench()}, clocks=CLOCKS)
    return result


def _rx_word(dut, ready):
    # The PHY presents one word every 4 cycles.
    yield dut.rx_valid.eq(1)
    yield dut.rx_ready.eq(ready)
    yield
    yield dut.rx_valid.eq(0)
    for _ in range(3):
        yield


def _tx_request(dut, valid):
    # The PHY asks for one word every 4 cycles.
    yield dut.tx_ready.eq(1)
    yield dut.tx_valid.eq(valid)
    yield
    yield dut.tx_ready.eq(0)
    for _ in range(3):
        yield


def test_rx_overflow_counts_one_event_per_episode_and_words_by_16():
    dut = AD9361StreamCounters()

    def gen():
        for _ in range(8):
            yield from _rx_word(dut, ready=1)
        for _ in range(40):                     # Episode 1: 40 words lost.
            yield from _rx_word(dut, ready=0)
        for _ in range(8):
            yield from _rx_word(dut, ready=1)
        for _ in range(8):                      # Episode 2: 8 words lost (48 in total).
            yield from _rx_word(dut, ready=0)
        yield from _rx_word(dut, ready=1)

    r = _run(dut, gen)
    assert r["rx_events"] == 2
    assert r["rx_words"]  == 48
    assert r["tx_events"] == 0


def test_rx_taken_words_are_not_counted():
    dut = AD9361StreamCounters()

    def gen():
        for _ in range(64):
            yield from _rx_word(dut, ready=1)

    r = _run(dut, gen)
    assert r["rx_events"] == 0 and r["rx_words"] == 0


def test_tx_underflow_counts_interruptions_not_idle():
    dut = AD9361StreamCounters()

    def gen():
        for _ in range(16):                     # Idle before any data: not an underflow.
            yield from _tx_request(dut, valid=0)
        for _ in range(16):
            yield from _tx_request(dut, valid=1)
        for _ in range(16):                     # Interruption 1 (one event, however long).
            yield from _tx_request(dut, valid=0)
        for _ in range(4):
            yield from _tx_request(dut, valid=1)
        yield from _tx_request(dut, valid=0)    # Interruption 2.
        yield from _tx_request(dut, valid=1)

    r = _run(dut, gen)
    assert r["tx_events"] == 2
    assert r["rx_events"] == 0
