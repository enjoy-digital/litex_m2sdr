#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import passive

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.udp import UDPPacketDropper


def _run(packets, room_at):
    """packets: lists of words; room_at(packet, word) -> room. Returns (words out, sink stalls, drops)."""
    dut = UDPPacketDropper([("data", 64)])
    out = []
    stalls = []

    def gen():
        yield dut.source.ready.eq(1)
        yield
        for n, packet in enumerate(packets):
            for i, word in enumerate(packet):
                yield dut.room.eq(room_at(n, i))
                yield dut.sink.valid.eq(1)
                yield dut.sink.data.eq(word)
                yield dut.sink.last.eq(i == len(packet) - 1)
                yield
                while not (yield dut.sink.ready):
                    stalls.append((n, i))
                    yield
        yield dut.sink.valid.eq(0)
        yield
        yield
        out.append(("drops", (yield dut.drops)))

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])
    drops = out.pop()[1]
    return out, stalls, drops


PACKETS = [[0x10, 0x11, 0x12], [0x20, 0x21, 0x22], [0x30, 0x31, 0x32]]


def test_room_passes_every_packet():
    out, stalls, drops = _run(PACKETS, lambda n, i: 1)
    assert [w for w, _ in out] == [w for p in PACKETS for w in p]
    assert [l for _, l in out] == [0, 0, 1] * 3
    assert stalls == [] and drops == 0


def test_no_room_drops_the_whole_packet_without_stalling():
    out, stalls, drops = _run(PACKETS, lambda n, i: n != 1)
    assert [w for w, _ in out] == PACKETS[0] + PACKETS[2]
    assert stalls == [] and drops == 1


def test_decision_is_held_for_the_packet():
    # Room lost after the first word: the packet still passes whole. Room back after the first
    # word of a dropped packet: the rest of it is still dropped.
    out, stalls, drops = _run(PACKETS, lambda n, i: (i == 0) if n == 0 else (i != 0) if n == 1 else 1)
    assert [w for w, _ in out] == PACKETS[0] + PACKETS[2]
    assert stalls == [] and drops == 1
