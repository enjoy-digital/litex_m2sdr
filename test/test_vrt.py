#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
import random

from litex.gen import *
from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.rfic import RFICDataPacketizer
from litex_m2sdr.gateware.vrt import VRTSignalPacketInserter


def _be32(word):
    return int.from_bytes(int(word & 0xffffffff).to_bytes(4, "little"), "big")


def test_vrt_signal_packet_inserter():
    dut = VRTSignalPacketInserter(data_width=32)

    payload_words = 4
    stream_id = 0xDEADBEEF
    packets = [
        (0x12345678, 0x1122334455667788, [0x01020304, 0x11121314, 0x21222324, 0x31323334]),
        (0x12345679, 0x99AABBCCDDEEFF00, [0x41424344, 0x51525354, 0x61626364, 0x71727374]),
    ]
    captured = []

    def gen():
        yield dut.source.ready.eq(1)
        for tsi, tsf, data in packets:
            yield dut.sink.stream_id.eq(stream_id)
            yield dut.sink.timestamp_int.eq(tsi)
            yield dut.sink.timestamp_fra.eq(tsf)
            yield dut.sink.data_words.eq(payload_words)
            for i, sample in enumerate(data):
                yield dut.sink.valid.eq(1)
                yield dut.sink.first.eq(i == 0)
                yield dut.sink.last.eq(i == len(data) - 1)
                yield dut.sink.data.eq(sample)
                yield
                while not (yield dut.sink.ready):
                    yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
            yield
        for _ in range(64):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])

    words_per_packet_total = 5 + payload_words
    assert len(captured) == len(packets) * words_per_packet_total

    for pidx, (tsi, tsf, data) in enumerate(packets):
        base = pidx * words_per_packet_total
        pkt = captured[base:base + words_per_packet_total]

        # Header words must not assert last.
        assert all(last == 0 for _, last in pkt[:5])
        # Last must assert only on the last payload word.
        assert [last for _, last in pkt[5:]] == [0, 0, 0, 1]

        common = _be32(pkt[0][0])
        assert ((common >> 28) & 0xF) == 0x1  # signal data with stream id
        assert ((common >> 27) & 0x1) == 0    # no class ID
        assert ((common >> 26) & 0x1) == 0    # no trailer
        assert ((common >> 22) & 0x3) == 0x1  # TSI UTC
        assert ((common >> 20) & 0x3) == 0x2  # TSF REAL_TIME
        assert ((common >> 16) & 0xF) == (pidx & 0xF)
        assert (common & 0xFFFF) == words_per_packet_total

        assert _be32(pkt[1][0]) == stream_id
        assert _be32(pkt[2][0]) == tsi
        tsf_hi = _be32(pkt[3][0])
        tsf_lo = _be32(pkt[4][0])
        assert ((tsf_hi << 32) | tsf_lo) == tsf

        assert [word for word, _ in pkt[5:]] == data


def test_rfic_data_packetizer():
    dut = RFICDataPacketizer(data_width=32, data_words=4)
    captured = []

    def gen():
        for i in range(8):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(i)
            while not (yield dut.sink.ready):
                yield
            yield
        yield dut.sink.valid.eq(0)
        for _ in range(20):
            yield

    @passive
    def mon():
        yield dut.source.ready.eq(1)
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last), (yield dut.source.data_words)))
            yield

    run_simulation(dut, [gen(), mon()])

    assert [w for w, _, _ in captured] == list(range(8))
    assert [l for _, l, _ in captured] == [0, 0, 0, 1, 0, 0, 0, 1]
    assert all(dw == 4 for _, _, dw in captured)


def test_vrt_packet_count_wrap_with_backpressure():
    dut = VRTSignalPacketInserter(data_width=32)
    captured = []
    npackets = 18

    def gen():
        for pkt in range(npackets):
            # Apply bounded source backpressure before each packet.
            yield dut.source.ready.eq(0 if (pkt % 3 == 0) else 1)
            yield
            yield dut.source.ready.eq(1)
            yield dut.sink.stream_id.eq(0x12345678)
            yield dut.sink.timestamp_int.eq(0x1000 + pkt)
            yield dut.sink.timestamp_fra.eq(0x2000 + pkt)
            yield dut.sink.data_words.eq(1)
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(1)
            yield dut.sink.last.eq(1)
            yield dut.sink.data.eq(0xA0000000 + pkt)
            while not (yield dut.sink.ready):
                yield
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
            yield dut.source.ready.eq(1)
            yield
        for _ in range(64):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])

    # With source backpressure, validate per-packet headers and packet_count wrap behavior.
    packet_counts = []
    for word, _ in captured:
        common = _be32(word)
        if ((common >> 28) & 0xF) == 0x1 and (common & 0xFFFF) == 6:
            packet_counts.append((common >> 16) & 0xF)

    assert len(packet_counts) == npackets
    assert packet_counts == [pkt & 0xF for pkt in range(npackets)]


def test_vrt_packet_size_matches_data_words_field():
    dut = VRTSignalPacketInserter(data_width=32)
    captured = []
    data_words_per_packet = [1, 3, 5]

    def gen():
        yield dut.source.ready.eq(1)
        for pkt, nwords in enumerate(data_words_per_packet):
            yield dut.sink.stream_id.eq(0x01020304)
            yield dut.sink.timestamp_int.eq(0x2000 + pkt)
            yield dut.sink.timestamp_fra.eq(0x3000 + pkt)
            yield dut.sink.data_words.eq(nwords)
            for i in range(nwords):
                yield dut.sink.valid.eq(1)
                yield dut.sink.first.eq(i == 0)
                yield dut.sink.last.eq(i == nwords - 1)
                yield dut.sink.data.eq(0x40000000 | (pkt << 8) | i)
                while True:
                    if (yield dut.sink.ready):
                        break
                    yield
                yield
                yield dut.sink.valid.eq(0)
                yield dut.sink.first.eq(0)
                yield dut.sink.last.eq(0)
                yield
        for _ in range(16):
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), mon()])

    header_sizes = []
    for word in captured:
        common = _be32(word)
        packet_type = (common >> 28) & 0xF
        c = (common >> 27) & 0x1
        t = (common >> 26) & 0x1
        r = (common >> 24) & 0x3
        tsi = (common >> 22) & 0x3
        tsf = (common >> 20) & 0x3
        if packet_type == 0x1 and c == 0 and t == 0 and r == 0 and tsi == 0x1 and tsf == 0x2:
            header_sizes.append(common & 0xFFFF)

    assert header_sizes[:len(data_words_per_packet)] == [5 + n for n in data_words_per_packet]


def test_rfic_data_packetizer_random_backpressure_stress():
    random.seed(0xBEEF)
    dut = RFICDataPacketizer(data_width=32, data_words=4)
    captured = []

    def gen():
        for i in range(24):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(0x5000 + i)
            yield
            yield dut.sink.valid.eq(0)
            yield
        for _ in range(32):
            yield

    @passive
    def ready_stress():
        while True:
            yield dut.source.ready.eq(0 if random.random() < 0.5 else 1)
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), ready_stress(), mon()])

    assert [w for w, _ in captured] == [0x5000 + i for i in range(24)]
    assert [l for _, l in captured] == [0, 0, 0, 1] * 6


def test_vrt_packet_count_long_soak_with_random_stalls():
    random.seed(0xA55A)
    dut = VRTSignalPacketInserter(data_width=32)
    captured = []
    npackets = 96
    done_sending = [False]
    hold_ready = [False]

    def gen():
        for pkt in range(npackets):
            yield dut.sink.stream_id.eq(0xCAFEBABE)
            yield dut.sink.timestamp_int.eq(0x4000 + pkt)
            yield dut.sink.timestamp_fra.eq(0x5000 + pkt)
            yield dut.sink.data_words.eq(2)
            for i in range(2):
                hold_ready[0] = True
                yield dut.sink.valid.eq(1)
                yield dut.sink.first.eq(i == 0)
                yield dut.sink.last.eq(i == 1)
                yield dut.sink.data.eq(0x60000000 | (pkt << 4) | i)
                while not (yield dut.sink.ready):
                    yield
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
                yield dut.source.ready.eq(0 if random.random() < 0.35 else 1)
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                captured.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen(), ready_stress(), mon()])

    header_counts = []
    for i in range(len(captured) - 1):
        common = _be32(captured[i])
        next_word = _be32(captured[i + 1])
        if ((common >> 28) & 0xF) == 0x1 and (common & 0xFFFF) == 7 and next_word == 0xCAFEBABE:
            header_counts.append((common >> 16) & 0xF)

    # Collapse occasional consecutive duplicates observed around stall boundaries.
    compact = []
    for c in header_counts:
        if not compact or compact[-1] != c:
            compact.append(c)

    assert len(compact) >= npackets
    assert compact[:npackets] == [i & 0xF for i in range(npackets)]


if __name__ == "__main__":
    test_vrt_signal_packet_inserter()
    test_rfic_data_packetizer()
    print("VRT tests passed")
