#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex_m2sdr.gateware.usb3 import USB3RXStreamer, USB3TXStreamer

# Helpers ------------------------------------------------------------------------------------------

def _send(ep, words):
    """Stream contract: present the beat, then hold valid until accepted."""
    for word in words:
        yield ep.valid.eq(1)
        yield ep.data.eq(word)
        yield
        while not (yield ep.ready):
            yield
    yield ep.valid.eq(0)


def _collect(ep, n, out, max_cycles=100000):
    yield ep.ready.eq(1)
    for _ in range(max_cycles):
        if len(out) >= n:
            break
        yield
        if (yield ep.valid) and (yield ep.ready):
            out.append((yield ep.data))
    yield ep.ready.eq(0)

# RX Streamer --------------------------------------------------------------------------------------

def test_usb3_rx_streamer_splits_64bit_words_low_word_first():
    dut = USB3RXStreamer(data_width=64, fifo_depth=16)
    words = [0x0706050403020100, 0x0f0e0d0c0b0a0908]
    out   = []
    flush = []

    def gen():
        yield dut.enable.eq(1)
        yield
        yield from _send(dut.sink, words)

    def flush_mon():
        for _ in range(8):
            flush.append((yield dut.flush))
            yield

    run_simulation(dut, [gen(), _collect(dut.source, 4, out), flush_mon()])
    assert out == [0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c]
    # Flush pulse on enable (BulkIN packets of a previous run dropped).
    assert sum(flush) == 1


def test_usb3_rx_streamer_drains_while_disabled():
    dut = USB3RXStreamer(data_width=64, fifo_depth=16)
    out = []

    def gen():
        # Disabled: accepted without back-pressure, nothing forwarded.
        yield from _send(dut.sink, list(range(32)))

    run_simulation(dut, [gen(), _collect(dut.source, 1, out, max_cycles=200)])
    assert out == []

# TX Streamer --------------------------------------------------------------------------------------

def test_usb3_tx_streamer_primes_then_merges_32bit_words():
    depth = 16
    dut   = USB3TXStreamer(data_width=64, fifo_depth=depth)
    words = list(range(1, 2*depth + 1)) # depth 64-bit words.
    out   = []
    started_before_prime = []

    def gen():
        yield dut.enable.eq(1)
        yield
        yield from _send(dut.sink, words[:depth - 2])
        for _ in range(16):
            yield
        started_before_prime.append((yield dut.started))
        yield from _send(dut.sink, words[depth - 2:])

    def rx():
        # Not forwarded before the FIFO is primed (half full).
        for _ in range(40):
            yield
        yield from _collect(dut.source, depth, out)

    run_simulation(dut, [gen(), rx()])
    assert started_before_prime == [0]
    assert out == [(words[2*i + 1] << 32) | words[2*i] for i in range(depth)]


def test_usb3_tx_streamer_drains_bulk_out_while_disabled():
    dut = USB3TXStreamer(data_width=64, fifo_depth=16)
    out = []

    def gen():
        yield from _send(dut.sink, list(range(64)))

    run_simulation(dut, [gen(), _collect(dut.source, 1, out, max_cycles=300)])
    assert out == []


def _collect_first(ep, n, out, max_cycles=100000):
    yield ep.ready.eq(1)
    for _ in range(max_cycles):
        if len(out) >= n:
            break
        yield
        if (yield ep.valid) and (yield ep.ready):
            out.append(((yield ep.data), (yield ep.first)))
    yield ep.ready.eq(0)


def test_usb3_tx_streamer_marks_the_first_word_of_each_frame():
    # 32-byte frames: 4 words of 64 bits, 8 Bulk OUT words of 32 bits.
    dut = USB3TXStreamer(data_width=64, fifo_depth=16, frame_bytes=32)
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield
        yield from _send(dut.sink, list(range(24)))

    run_simulation(dut, [gen(), _collect_first(dut.source, 12, out)])
    assert [first for _, first in out] == [1, 0, 0, 0]*3
    assert out[0][0] == 0x0000000100000000
    assert out[4][0] == 0x0000000900000008


def test_usb3_tx_streamer_realigns_frames_on_enable():
    dut = USB3TXStreamer(data_width=64, fifo_depth=16, frame_bytes=32)
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.framed.eq(1)
        yield
        yield from _send(dut.sink, list(range(6)))      # 3 words: stops inside a frame.
        for _ in range(16):
            yield
        yield dut.enable.eq(0)
        yield
        yield
        yield dut.enable.eq(1)
        yield
        yield from _send(dut.sink, list(range(100, 116)))

    run_simulation(dut, [gen(), _collect_first(dut.source, 11, out)])
    assert [first for _, first in out] == [1, 0, 0] + [1, 0, 0, 0]*2
    assert out[3][0] == 0x0000006500000064


def test_usb3_tx_streamer_framed_forwards_without_priming():
    dut = USB3TXStreamer(data_width=64, fifo_depth=16, frame_bytes=32)
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.framed.eq(1)
        yield
        yield from _send(dut.sink, [1, 2])              # One word: far below the half-full level.

    run_simulation(dut, [gen(), _collect_first(dut.source, 1, out, max_cycles=64)])
    assert out == [(0x0000000200000001, 1)]
