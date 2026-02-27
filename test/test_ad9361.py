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

from litex_m2sdr.gateware.ad9361.bitmode import AD9361RXBitMode, AD9361TXBitMode
from litex_m2sdr.gateware.ad9361.prbs import AD9361PRBSChecker, AD9361PRBSGenerator

# AD9361 BitMode Tests ----------------------------------------------------------------------------


def test_ad9361_tx_bitmode_16bit_passthrough():
    dut = AD9361TXBitMode()
    out = []

    def gen():
        yield dut.mode.eq(0)
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x1122334455667788)
        yield
        yield dut.sink.valid.eq(0)
        yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert out == [0x1122334455667788]


def test_ad9361_rx_bitmode_16bit_passthrough():
    dut = AD9361RXBitMode()
    out = []

    def gen():
        yield dut.mode.eq(0)
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x99AABBCCDDEEFF00)
        yield
        yield dut.sink.valid.eq(0)
        yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert out == [0x99AABBCCDDEEFF00]


def test_ad9361_tx_bitmode_8bit_sign_extension():
    dut = AD9361TXBitMode()
    out = []

    def gen():
        yield dut.mode.eq(1)
        yield dut.source.ready.eq(1)
        # Keep valid high for two cycles to emit both 32-bit halves.
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x807F00FF11223344)
        yield
        yield
        yield dut.sink.valid.eq(0)
        for _ in range(4):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert 0x0110022003300440 in out
    assert 0xF80007F00000FFF0 in out


def test_ad9361_rx_bitmode_8bit_packing():
    dut = AD9361RXBitMode()
    out = []

    def gen():
        yield dut.mode.eq(1)
        yield dut.source.ready.eq(1)
        # Two 64-bit beats are required to emit one packed 64-bit beat.
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(0)
        yield dut.sink.data.eq((0x00C << 4) | ((0x00D << 4) << 16) | ((0x00E << 4) << 32) | ((0x00F << 4) << 48))
        yield
        yield dut.sink.first.eq(0)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq((0x008 << 4) | ((0x009 << 4) << 16) | ((0x00A << 4) << 32) | ((0x00B << 4) << 48))
        yield
        yield dut.sink.valid.eq(0)
        for _ in range(4):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])
    assert out == [0x0B0A09080F0E0D0C]

# AD9361 PRBS Tests -------------------------------------------------------------------------------


def test_ad9361_prbs_checker_sync_and_loss():
    gen = AD9361PRBSGenerator(seed=0x0A54)
    chk = AD9361PRBSChecker(seed=0x0A54)
    dut = Module()
    dut.submodules += gen, chk
    synced_trace = []

    def stimulus():
        for _ in range(1200):
            yield chk.i.eq((yield gen.o) & 0xFFF)
            synced_trace.append((yield chk.synced))
            yield
        # Inject wrong samples to force loss of sync.
        for _ in range(4):
            yield chk.i.eq(0x000)
            synced_trace.append((yield chk.synced))
            yield

    run_simulation(dut, stimulus())
    assert any(synced_trace[1100:])
    assert synced_trace[-1] == 0


def test_ad9361_prbs_generator_ce_hold():
    dut = AD9361PRBSGenerator(seed=0x0A54)
    values = []

    def gen():
        for _ in range(4):
            values.append((yield dut.o))
            yield
        yield dut.ce.eq(0)
        for _ in range(4):
            values.append((yield dut.o))
            yield

    run_simulation(dut, gen())
    assert len(set(values[-3:])) == 1


def test_ad9361_tx_bitmode_mode_switch_mid_stream():
    random.seed(0x55AA)
    dut = AD9361TXBitMode()
    out = []

    def gen():
        yield dut.source.ready.eq(1)
        # Start in 16-bit mode.
        yield dut.mode.eq(0)
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.data.eq(0x0123456789ABCDEF)
        yield
        # Switch to 8-bit mode and keep valid for two cycles to emit both halves.
        yield dut.mode.eq(1)
        yield dut.sink.data.eq(0x807F00FF11223344)
        yield
        yield
        # Random ready stalls while data is idle.
        yield dut.sink.valid.eq(0)
        for _ in range(8):
            yield dut.source.ready.eq(0 if random.random() < 0.3 else 1)
            yield
        yield dut.source.ready.eq(1)
        for _ in range(4):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])

    # Must contain one 16-bit passthrough beat and 8-bit converted beats.
    assert 0x0123456789ABCDEF in out
    assert 0x0110022003300440 in out
    assert 0xF80007F00000FFF0 in out


def test_ad9361_prbs_checker_stays_unsynced_after_error_burst():
    gen = AD9361PRBSGenerator(seed=0x0A54)
    chk = AD9361PRBSChecker(seed=0x0A54)
    dut = Module()
    dut.submodules += gen, chk
    synced = []

    def stimulus():
        # Initial lock.
        for _ in range(1200):
            yield chk.i.eq((yield gen.o) & 0xFFF)
            synced.append((yield chk.synced))
            yield
        # Error burst.
        for _ in range(12):
            yield chk.i.eq(0x000)
            synced.append((yield chk.synced))
            yield
        # Keep feeding generator output after error burst.
        for _ in range(1300):
            yield chk.i.eq((yield gen.o) & 0xFFF)
            synced.append((yield chk.synced))
            yield

    run_simulation(dut, stimulus())
    assert any(synced[1100:1200])  # initially locked
    assert synced[1210] == 0        # lost lock after error burst
    assert not any(synced[-200:])   # remains out-of-lock with free-running source
