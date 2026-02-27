#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive
import random

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.loopback import TXRXLoopback


def test_txrx_loopback_normal_and_loopback():
    dut = TXRXLoopback(data_width=64, with_csr=False)
    tx_out = []
    rx_out = []

    def gen():
        yield dut.tx_source.ready.eq(1)
        yield dut.rx_source.ready.eq(1)

        # Normal mode.
        yield dut.enable.eq(0)
        yield dut.tx_sink.valid.eq(1)
        yield dut.tx_sink.first.eq(1)
        yield dut.tx_sink.last.eq(1)
        yield dut.tx_sink.data.eq(0xAAAA)
        yield dut.rx_sink.valid.eq(1)
        yield dut.rx_sink.first.eq(1)
        yield dut.rx_sink.last.eq(1)
        yield dut.rx_sink.data.eq(0xBBBB)
        yield
        yield dut.tx_sink.valid.eq(0)
        yield dut.rx_sink.valid.eq(0)
        yield

        # Loopback mode.
        yield dut.enable.eq(1)
        yield dut.tx_sink.valid.eq(1)
        yield dut.tx_sink.first.eq(1)
        yield dut.tx_sink.last.eq(1)
        yield dut.tx_sink.data.eq(0xCCCC)
        yield dut.rx_sink.valid.eq(1)
        yield dut.rx_sink.data.eq(0xDDDD)  # must be ignored/drained.
        yield
        yield dut.tx_sink.valid.eq(0)
        yield dut.rx_sink.valid.eq(0)
        for _ in range(4):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.tx_source.valid) and (yield dut.tx_source.ready):
                tx_out.append((yield dut.tx_source.data))
            if (yield dut.rx_source.valid) and (yield dut.rx_source.ready):
                rx_out.append((yield dut.rx_source.data))
            yield

    run_simulation(dut, [gen(), mon()])

    assert tx_out == [0xAAAA]
    assert rx_out == [0xBBBB, 0xCCCC]


def test_txrx_loopback_drains_rx_when_enabled():
    dut = TXRXLoopback(data_width=64, with_csr=False)
    rx_ready_trace = []

    def gen():
        yield dut.enable.eq(1)
        yield
        for _ in range(4):
            yield dut.rx_sink.valid.eq(1)
            yield dut.rx_sink.data.eq(0x1234)
            yield
            rx_ready_trace.append((yield dut.rx_sink.ready))
        yield dut.rx_sink.valid.eq(0)
        yield

    run_simulation(dut, gen())
    assert all(v == 1 for v in rx_ready_trace)


def test_txrx_loopback_disables_tx_source_when_enabled():
    dut = TXRXLoopback(data_width=64, with_csr=False)
    tx_valid = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.tx_source.ready.eq(1)
        yield
        for _ in range(3):
            yield dut.tx_sink.valid.eq(1)
            yield dut.tx_sink.first.eq(1)
            yield dut.tx_sink.last.eq(1)
            yield dut.tx_sink.data.eq(0xFACE)
            yield
            tx_valid.append((yield dut.tx_source.valid))
        yield dut.tx_sink.valid.eq(0)
        yield

    run_simulation(dut, gen())
    assert all(v == 0 for v in tx_valid)


def test_txrx_loopback_mode_toggle_mid_stream():
    random.seed(0x1234)
    dut = TXRXLoopback(data_width=64, with_csr=False)
    tx_out = []
    rx_out = []

    def gen():
        yield dut.tx_source.ready.eq(1)
        yield dut.rx_source.ready.eq(1)
        for i in range(16):
            # Toggle loopback mode every 4 beats.
            yield dut.enable.eq(1 if ((i // 4) % 2) else 0)
            yield dut.tx_sink.valid.eq(1)
            yield dut.tx_sink.first.eq(1)
            yield dut.tx_sink.last.eq(1)
            yield dut.tx_sink.data.eq(0x1000 + i)
            yield dut.rx_sink.valid.eq(1)
            yield dut.rx_sink.first.eq(1)
            yield dut.rx_sink.last.eq(1)
            yield dut.rx_sink.data.eq(0x2000 + i)
            # Random consumer stalls.
            yield dut.tx_source.ready.eq(0 if random.random() < 0.25 else 1)
            yield dut.rx_source.ready.eq(0 if random.random() < 0.25 else 1)
            yield
        yield dut.tx_sink.valid.eq(0)
        yield dut.rx_sink.valid.eq(0)
        yield dut.tx_source.ready.eq(1)
        yield dut.rx_source.ready.eq(1)
        for _ in range(16):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.tx_source.valid) and (yield dut.tx_source.ready):
                tx_out.append((yield dut.tx_source.data))
            if (yield dut.rx_source.valid) and (yield dut.rx_source.ready):
                rx_out.append((yield dut.rx_source.data))
            yield

    run_simulation(dut, [gen(), mon()])

    # In normal windows, RX path should expose incoming rx_sink words.
    assert any((0x2000 <= w < 0x2010) for w in rx_out)
    # In loopback windows, RX path should include tx_sink words.
    assert any((0x1000 <= w < 0x1010) for w in rx_out)


def test_txrx_loopback_mode_toggle_reference_mapping():
    dut = TXRXLoopback(data_width=64, with_csr=False)
    tx_out = []
    rx_out = []
    expected_tx = []
    expected_rx = []

    def gen():
        yield dut.tx_source.ready.eq(1)
        yield dut.rx_source.ready.eq(1)
        for i in range(24):
            mode = 1 if (i % 3 == 0) else 0
            tx_word = 0x3000 + i
            rx_word = 0x4000 + i
            yield dut.enable.eq(mode)
            yield dut.tx_sink.valid.eq(1)
            yield dut.tx_sink.first.eq(1)
            yield dut.tx_sink.last.eq(1)
            yield dut.tx_sink.data.eq(tx_word)
            yield dut.rx_sink.valid.eq(1)
            yield dut.rx_sink.first.eq(1)
            yield dut.rx_sink.last.eq(1)
            yield dut.rx_sink.data.eq(rx_word)
            yield
            # With always-ready sinks and one-cycle pulses, routing is cycle-exact.
            if mode == 0:
                expected_tx.append(tx_word)
                expected_rx.append(rx_word)
            else:
                expected_rx.append(tx_word)
            yield dut.tx_sink.valid.eq(0)
            yield dut.rx_sink.valid.eq(0)
            yield
        for _ in range(8):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.tx_source.valid) and (yield dut.tx_source.ready):
                tx_out.append((yield dut.tx_source.data))
            if (yield dut.rx_source.valid) and (yield dut.rx_source.ready):
                rx_out.append((yield dut.rx_source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert tx_out == expected_tx
    assert rx_out == expected_rx
