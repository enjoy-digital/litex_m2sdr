#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex_m2sdr.gateware.header import TXHeaderPacketMarker

SYNC = 0x5aa5_5aa5_5aa5_5aa5


def _run(packets, reset_before=None):
    """packets: list of lists of words. Returns [(data, first)] seen on the source."""
    dut = TXHeaderPacketMarker(data_width=64)
    out = []

    def gen():
        yield dut.source.ready.eq(1)
        yield
        for n, packet in enumerate(packets):
            if reset_before == n:
                yield dut.sink.valid.eq(0)
                yield dut.reset.eq(1)
                yield
                yield dut.reset.eq(0)
            for i, word in enumerate(packet):
                yield dut.sink.valid.eq(1)
                yield dut.sink.data.eq(word)
                yield dut.sink.last.eq(i == len(packet) - 1)
                yield
                out.append(((yield dut.source.data), (yield dut.source.first)))
        yield dut.sink.valid.eq(0)
        yield

    run_simulation(dut, gen())
    return out


def test_first_on_a_packet_that_starts_with_the_sync_word():
    out = _run([[SYNC, 100, 1, 2], [3, 4, 5, 6], [SYNC, 200, 7, 8]])
    assert [first for _, first in out] == [1, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0]
    assert [data for data, _ in out][:2] == [SYNC, 100]


def test_sync_word_inside_a_packet_is_not_a_frame_start():
    out = _run([[SYNC, 100, SYNC, 2], [3, SYNC, 5, 6]])
    assert [first for _, first in out] == [1, 0, 0, 0,  0, 0, 0, 0]


def test_frame_found_again_after_a_lost_packet():
    # Buffers of 3 packets; the second packet of the first buffer is lost.
    out = _run([[SYNC, 100, 1, 2], [5, 6, 7, 8], [SYNC, 200, 9, 10], [11, 12, 13, 14], [15, 16, 17, 18]])
    assert [first for _, first in out] == [1, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0]


def test_reset_makes_the_next_word_a_packet_start():
    # The stream is disabled inside a packet: the next word starts a packet.
    out = _run([[1, 2], [SYNC, 300, 3, 4]], reset_before=1)
    assert [first for _, first in out] == [0, 0,  1, 0, 0, 0]
