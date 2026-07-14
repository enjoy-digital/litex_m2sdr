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

# Header Inserter/Extractor Tests -----------------------------------------------------------------


def test_header_inserter():
    """Verify inserter emits header/timestamp then payload with correct framing flags."""
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
    """Verify extractor captures header/timestamp and forwards payload framing correctly."""
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
        # Release the timed-TX gate immediately: FPGA time == air-time -> forward the frame
        # the cycle it is due (the extractor holds each frame until dut.time reaches the
        # captured timestamp; see the gate tests below).
        yield dut.time.eq(timestamp)

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
    """Verify inserter is payload passthrough when header insertion is disabled."""
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
    """Stress inserter under random output backpressure and check frame boundaries."""
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


def test_header_inserter_frame_invariants_per_cycle():
    """Check per-cycle invariants for first frame: header, timestamp, payload, last."""
    dut = HeaderInserterExtractor(mode="inserter", data_width=64, with_csr=False)
    payload = [0xAA, 0xBB, 0xCC, 0xDD]
    accepted = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(len(payload))
        yield dut.header.eq(0x1111111122222222)
        yield dut.timestamp.eq(0x3333333344444444)
        yield dut.source.ready.eq(1)
        for i, w in enumerate(payload):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == len(payload) - 1)
            yield dut.sink.data.eq(w)
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
                accepted.append({
                    "data":  (yield dut.source.data),
                    "first": (yield dut.source.first),
                    "last":  (yield dut.source.last),
                })
            yield

    run_simulation(dut, [gen(), mon()])

    # First frame structure invariant: [header, timestamp, payload...last]
    frame = accepted[:2 + len(payload)]
    assert len(frame) == 2 + len(payload)
    assert frame[0]["data"] == 0x1111111122222222 and frame[0]["first"] == 1 and frame[0]["last"] == 0
    assert frame[1]["data"] == 0x3333333344444444 and frame[1]["first"] == 0 and frame[1]["last"] == 0
    assert [x["data"] for x in frame[2:]] == payload
    assert [x["first"] for x in frame[2:]] == [0] * len(payload)
    assert [x["last"] for x in frame[2:]] == [0, 0, 0, 1]


def test_header_inserter_zero_frame_cycles_behaves_as_single_word_frames():
    """Verify frame_cycles=0 corner case behaves as one-word payload frames."""
    dut = HeaderInserterExtractor(mode="inserter", data_width=64, with_csr=False)
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(0)  # Corner case under test.
        yield dut.header.eq(0xAAAA0000AAAA0000)
        yield dut.timestamp.eq(0xBBBB0000BBBB0000)
        yield dut.source.ready.eq(1)

        for i, w in enumerate([0x10, 0x11]):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(1)
            yield dut.sink.data.eq(w)
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
                out.append(((yield dut.source.data), (yield dut.source.last)))
            yield

    run_simulation(dut, [gen(), mon()])

    # Expect: header/timestamp before each payload beat, and payload beat marked last.
    first_frame = out[:3]
    second_frame = out[3:6]
    assert [w for w, _ in first_frame] == [0xAAAA0000AAAA0000, 0xBBBB0000BBBB0000, 0x10]
    assert [l for _, l in first_frame] == [0, 0, 1]
    assert [w for w, _ in second_frame] == [0xAAAA0000AAAA0000, 0xBBBB0000BBBB0000, 0x11]
    assert [l for _, l in second_frame] == [0, 0, 1]


def test_header_extractor_header_disabled_passthrough():
    """Verify extractor is payload passthrough when header extraction is disabled."""
    dut = HeaderInserterExtractor(mode="extractor", data_width=64, with_csr=False)
    out = []

    def gen():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(0)
        yield dut.frame_cycles.eq(4)
        yield dut.source.ready.eq(1)

        for i, word in enumerate([0x20, 0x21, 0x22, 0x23]):
            while not (yield dut.sink.ready):
                yield
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == 3)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
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

    assert [w for w, _, _ in out] == [0x20, 0x21, 0x22, 0x23]
    assert [f for _, f, _ in out] == [0, 0, 0, 0]
    assert [l for _, _, l in out] == [0, 0, 0, 1]


# Timed-TX Gate (extractor) Tests ------------------------------------------------------------------
#
# The extractor holds each captured frame until (dut.time + dut.tx_offset) reaches the frame's
# air-time (the captured header timestamp), then forwards it. timestamp 0 is the "untimed /
# transmit immediately" sentinel; a frame already past its air-time on arrival is dropped whole
# and counted in dut.underflow. Payload is forwarded only after the gate releases.

def _gate_frame_words(data_width, header, timestamp, payload):
    """Wire words for one framed buffer: 128-bit packs [timestamp|sync] into one header word,
    64-bit uses a sync word then a timestamp word."""
    if data_width == 128:
        return [((timestamp << 64) | header)] + list(payload)
    return [header, timestamp] + list(payload)


def _run_gate(data_width, timestamp, time_start, time_release=None, tx_offset=0, frame_cycles=3):
    """Push one framed buffer through the extractor gate. dut.time starts at time_start and,
    if time_release is given, is raised to it after the frame is captured (models the FPGA
    time crossing the air-time while the frame is held). Returns (out_payload, underflow)."""
    dut = HeaderInserterExtractor(mode="extractor", data_width=data_width, with_csr=False)
    header  = 0xA1A2A3A4A5A6A7A8
    payload = [0x300 + i for i in range(frame_cycles)]
    words   = _gate_frame_words(data_width, header, timestamp, payload)
    out     = []
    result  = {}

    def gen_sink():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(frame_cycles)
        yield dut.tx_offset.eq(tx_offset)
        yield dut.source.ready.eq(1)
        for i, word in enumerate(words):
            guard = 0
            while not (yield dut.sink.ready):
                yield
                guard += 1
                assert guard < 500, "gate never accepted the payload (stuck in WAIT?)"
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(i == 0)
            yield dut.sink.last.eq(i == len(words) - 1)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield dut.sink.last.eq(0)
            yield
        for _ in range(12):
            yield
        result["underflow"] = (yield dut.underflow)

    @passive
    def gen_time():
        yield dut.time.eq(time_start)
        for _ in range(24):
            yield
        if time_release is not None:
            yield dut.time.eq(time_release)
        while True:
            yield

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen_sink(), gen_time(), mon()])
    return out, result.get("underflow", 0)


def test_gate_untimed_passthrough():
    """timestamp 0 (untimed) transmits immediately, even with FPGA time far ahead."""
    for dw in (64, 128):
        payload = [0x300, 0x301, 0x302]
        out, underflow = _run_gate(dw, timestamp=0, time_start=1_000_000)
        assert out == payload, (dw, out)
        assert underflow == 0, (dw, underflow)


def test_gate_release_at_exact_time():
    """A timed frame whose air-time equals the current FPGA time forwards immediately."""
    for dw in (64, 128):
        payload = [0x300, 0x301, 0x302]
        out, underflow = _run_gate(dw, timestamp=5000, time_start=5000)
        assert out == payload, (dw, out)
        assert underflow == 0, (dw, underflow)


def test_gate_hold_then_release():
    """A timed frame is held (no output) while FPGA time < air-time, then forwarded the
    moment time crosses the air-time."""
    for dw in (64, 128):
        payload = [0x300, 0x301, 0x302]
        # time starts below the air-time (held), then rises to exactly the air-time.
        out, underflow = _run_gate(dw, timestamp=5000, time_start=1000, time_release=5000)
        assert out == payload, (dw, out)
        assert underflow == 0, (dw, underflow)


def test_gate_drop_when_late():
    """A timed frame already past its air-time on arrival is dropped whole (no output) and
    counted as one TX underflow."""
    for dw in (64, 128):
        out, underflow = _run_gate(dw, timestamp=5000, time_start=9000)
        assert out == [], (dw, out)
        assert underflow == 1, (dw, underflow)


def test_gate_tx_offset_shifts_release():
    """tx_offset is added to FPGA time before comparing to the air-time: with time=air-time-
    offset the frame is due now and forwards."""
    for dw in (64, 128):
        payload = [0x300, 0x301, 0x302]
        out, underflow = _run_gate(dw, timestamp=5000, time_start=4000, tx_offset=1000)
        assert out == payload, (dw, out)
        assert underflow == 0, (dw, underflow)


def test_gate_underflow_accumulates_and_recovers():
    """After dropping a late frame the extractor returns to framing and forwards the next
    (untimed) frame; the underflow counter persists."""
    dut = HeaderInserterExtractor(mode="extractor", data_width=128, with_csr=False)
    header = 0xA1A2A3A4A5A6A7A8
    late   = _gate_frame_words(128, header, 5000, [0x300, 0x301, 0x302])   # air-time in the past
    live   = _gate_frame_words(128, header, 0,    [0x400, 0x401, 0x402])   # untimed -> immediate
    out    = []
    result = {}

    def gen_sink():
        yield dut.enable.eq(1)
        yield dut.header_enable.eq(1)
        yield dut.frame_cycles.eq(3)
        yield dut.time.eq(9000)   # both frames' timed check sees "now" past 5000
        yield dut.source.ready.eq(1)
        for i, word in enumerate(late + live):
            guard = 0
            while not (yield dut.sink.ready):
                yield
                guard += 1
                assert guard < 500
            first = (i == 0) or (i == len(late))
            yield dut.sink.valid.eq(1)
            yield dut.sink.first.eq(1 if first else 0)
            yield dut.sink.data.eq(word)
            yield
            yield dut.sink.valid.eq(0)
            yield dut.sink.first.eq(0)
            yield
        for _ in range(12):
            yield
        result["underflow"] = (yield dut.underflow)

    @passive
    def mon():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                out.append((yield dut.source.data))
            yield

    run_simulation(dut, [gen_sink(), mon()])
    assert out == [0x400, 0x401, 0x402], out   # only the untimed frame reached the air
    assert result["underflow"] == 1, result
