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

from litex_m2sdr.gateware.header import HeaderInserterExtractor


def test_header_inserter():
    dut = HeaderInserterExtractor(mode="inserter", data_width=64, with_csr=False)

    payload = [0x100, 0x101, 0x102]
    header = 0x1122334455667788
    timestamp = 0x0123456789ABCDEF
    captured = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(len(payload))
        yield dut.header.eq(header)
        yield dut.timestamp.eq(timestamp)
        yield dut.source.ready.eq(1)

        for i, word in enumerate(payload):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == len(payload) - 1)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
            yield

        for _ in range(8):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.first), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])

    assert [w for w, _, _ in captured[:5]] == [header, timestamp] + payload
    assert [f for _, f, _ in captured[:5]] == [1, 0, 0, 0, 0]
    assert [l for _, _, l in captured[:5]] == [0, 0, 0, 0, 1]


def test_header_extractor():
    dut = HeaderInserterExtractor(mode="extractor", data_width=64, with_csr=False)

    header = 0xA1A2A3A4A5A6A7A8
    timestamp = 0x0F1E2D3C4B5A6978
    payload = [0x200, 0x201, 0x202]
    out_payload = []
    updates = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(len(payload))
        yield dut.source.ready.eq(1)

        sequence = [header, timestamp] + payload
        for i, word in enumerate(sequence):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == len(sequence) - 1)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
            yield

        for _ in range(8):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.update):
                updates.append(((yield dut.header), (yield dut.timestamp)))
            if (yield dut.source.valid) and (yield dut.source.ready):
                out_payload.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])

    assert updates
    assert updates[-1] == (header, timestamp)
    assert [w for w, _ in out_payload[:3]] == payload
    assert [l for _, l in out_payload[:3]] == [0, 0, 1]


def test_header_inserter_header_disabled_passthrough():
    dut = HeaderInserterExtractor(mode="inserter", data_width=64, with_csr=False)
    payload = [0x10, 0x11, 0x12, 0x13]
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(0)
        yield dut.frame_cycles.eq(len(payload))
        yield dut.source.ready.eq(1)
        for i, word in enumerate(payload):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == len(payload) - 1)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield
        for _ in range(4):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append(((yield dut.source.data), (yield dut.source.first), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])
    assert [w for w, _, _ in out] == payload
    assert [f for _, f, _ in out] == [0, 0, 0, 0]
    assert [l for _, _, l in out] == [0, 0, 0, 1]


def test_header_inserter_random_backpressure_stress():
    random.seed(0xC0DE)
    dut = HeaderInserterExtractor(mode="inserter", data_width=64, with_csr=False)
    payload = [0x100 + i for i in range(12)]
    out = []
    done_sending = [False]
    hold_ready = [False]

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(len(payload))
        yield dut.header.eq(0x1234567890ABCDEF)
        yield dut.timestamp.eq(0x0BADF00DCAFEBABE)

        for i, word in enumerate(payload):
            while not (yield dut.sink.ready):
                yield
            hold_ready[0] = True
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == len(payload) - 1)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
            hold_ready[0] = False
            yield

        done_sending[0] = True
        for _ in range(128):
            yield

    @passive
    def ready_stress():
        while True:
            if done_sending[0]:
                yield dut.source.ready.eq(1)
            elif hold_ready[0]:
                yield dut.source.ready.eq(1)
            else:
                yield dut.source.ready.eq(0 if random.random() < 0.25 else 1)
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), ready_stress(), mon()])

    # Detect frame boundaries from source.last and validate each frame structure.
    frames = []
    cur = []
    for w, l in out:
        cur.append(w)
        if l:
            frames.append(cur)
            cur = []
    assert frames
    first = frames[0]
    assert first[:2] == [0x1234567890ABCDEF, 0x0BADF00DCAFEBABE]
    assert first[2:2 + len(payload)] == payload
