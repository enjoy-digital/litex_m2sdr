#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import passive, run_simulation
from litesata.frontend.arbitration import LiteSATAUserPort

from litex_m2sdr.gateware.sata import (
    M2SDRLiteSATAStream2Sectors, M2SDRLiteSATASectors2Stream)


def write_stats():
    return {"command_counts": [], "write_cycles": [], "cycle": 0}


def make_write_controller(port, stats):
    """Accept streamer write commands and acknowledge each one at its last
    word, collecting the count of every command, the cycle of every accepted
    word, and a running cycle counter shared with the producers."""
    @passive
    def controller():
        acknowledging = False
        first_word = True
        while True:
            yield port.sink.ready.eq(1)
            if (yield port.sink.valid) and (yield port.sink.ready):
                if first_word:
                    stats["command_counts"].append((yield port.sink.count))
                    first_word = False
                stats["write_cycles"].append(stats["cycle"])
                if (yield port.sink.last):
                    acknowledging = True
                    first_word = True
            yield port.source.valid.eq(1 if acknowledging else 0)
            yield port.source.last.eq(1 if acknowledging else 0)
            yield port.source.failed.eq(0)
            if acknowledging and (yield port.source.ready):
                acknowledging = False
            stats["cycle"] += 1
            yield
    return controller


def test_stream_to_sata_waits_for_and_writes_a_contiguous_burst():
    port = LiteSATAUserPort(32)
    dut = M2SDRLiteSATAStream2Sectors(
        port, data_width=64, burst_sectors=2, fifo_sectors=4)
    stats = write_stats()
    producer_done_cycle = None

    def producer():
        nonlocal producer_done_cycle
        yield dut.sector.storage.eq(0x1234)
        yield dut.nsectors.storage.eq(2)
        yield dut.start.re.eq(1)
        yield
        yield dut.start.re.eq(0)
        for value in range(2 * 512 // 8):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(value)
            while not (yield dut.sink.ready):
                yield
            yield
        yield dut.sink.valid.eq(0)
        producer_done_cycle = stats["cycle"]
        for _ in range(700):
            if (yield dut.done.status):
                break
            yield
        assert (yield dut.done.status) == 1
        assert (yield dut.error.status) == 0
        assert (yield dut.progress.status) == 2

    run_simulation(dut, [producer(), make_write_controller(port, stats)()])
    write_cycles = stats["write_cycles"]
    assert stats["command_counts"] == [2]
    assert len(write_cycles) == 2 * 512 // 4
    assert write_cycles == list(range(write_cycles[0], write_cycles[0] + len(write_cycles)))
    assert producer_done_cycle is not None


def test_stream_to_sata_preloads_then_keeps_one_long_command_open():
    port = LiteSATAUserPort(32)
    dut = M2SDRLiteSATAStream2Sectors(
        port, data_width=64, burst_sectors=4, preload_sectors=2,
        fifo_sectors=4)
    stats = write_stats()
    producer_done_cycle = None

    def producer():
        nonlocal producer_done_cycle
        yield dut.nsectors.storage.eq(4)
        yield dut.start.re.eq(1)
        yield
        yield dut.start.re.eq(0)
        for value in range(4 * 512 // 8):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(value)
            while not (yield dut.sink.ready):
                yield
            yield
        yield dut.sink.valid.eq(0)
        producer_done_cycle = stats["cycle"]
        for _ in range(1200):
            if (yield dut.done.status):
                break
            yield
        assert (yield dut.done.status) == 1
        assert (yield dut.error.status) == 0

    run_simulation(dut, [producer(), make_write_controller(port, stats)()])
    assert stats["command_counts"] == [4]
    assert len(stats["write_cycles"]) == 4 * 512 // 4
    assert producer_done_cycle is not None
    assert stats["write_cycles"][0] < producer_done_cycle


def test_stream_to_sata_refills_between_contiguous_reservoir_bursts():
    port = LiteSATAUserPort(32)
    dut = M2SDRLiteSATAStream2Sectors(
        port, data_width=64, burst_sectors=4, preload_sectors=2,
        low_watermark_sectors=1, fifo_sectors=4)
    stats = write_stats()

    def sparse_producer():
        yield dut.nsectors.storage.eq(4)
        yield dut.start.re.eq(1)
        yield
        yield dut.start.re.eq(0)
        for value in range(4 * 512 // 8):
            yield dut.sink.valid.eq(1)
            yield dut.sink.data.eq(value)
            while not (yield dut.sink.ready):
                yield
            yield
            yield dut.sink.valid.eq(0)
            for _ in range(4):
                yield
        for _ in range(3000):
            if (yield dut.done.status):
                break
            yield
        assert (yield dut.done.status) == 1

    run_simulation(dut, [sparse_producer(), make_write_controller(port, stats)()])
    write_cycles = stats["write_cycles"]
    assert stats["command_counts"] == [4]
    assert len(write_cycles) == 4 * 512 // 4
    # The ATA command remains open, but valid pauses while the reservoir
    # refills instead of falling back to the sparse producer cadence.
    gaps = [b - a for a, b in zip(write_cycles, write_cycles[1:])]
    assert max(gaps) > 4
    assert sum(gap > 1 for gap in gaps) < (len(gaps) // 4)


def test_sata_to_stream_reads_a_burst_and_paces_output():
    port = LiteSATAUserPort(32)
    dut = M2SDRLiteSATASectors2Stream(
        port, data_width=64, sys_clk_freq=100,
        burst_sectors=2, fifo_sectors=4)
    command_counts = []
    output_cycles = []
    output_last = []
    cycle = 0

    def controller():
        yield port.sink.ready.eq(1)
        while not ((yield port.sink.valid) and (yield port.sink.ready)):
            yield
        command_counts.append((yield port.sink.count))
        yield

        for response_index in range(2 * 512 // 4):
            yield port.source.valid.eq(1)
            yield port.source.read.eq(1)
            yield port.source.end.eq(0)
            yield port.source.data.eq(response_index)
            # Two DATA FIS packets: neither packet boundary completes the
            # multi-sector command.
            yield port.source.last.eq(1 if response_index in (127, 255) else 0)
            yield port.source.failed.eq(0)
            while not (yield port.source.ready):
                yield
            yield
        yield port.source.end.eq(1)
        yield port.source.last.eq(1)
        while not (yield port.source.ready):
            yield
        yield
        yield port.source.valid.eq(0)
        yield port.source.read.eq(0)
        yield port.source.end.eq(0)
        yield port.source.last.eq(0)

    @passive
    def clock_monitor():
        nonlocal cycle
        while True:
            cycle += 1
            yield

    @passive
    def output_monitor():
        while True:
            if (yield dut.source.valid) and (yield dut.source.ready):
                output_cycles.append(cycle)
                output_last.append((yield dut.source.last))
            yield

    def consumer():
        yield dut.sector.storage.eq(0x2345)
        yield dut.nsectors.storage.eq(2)
        yield dut.pace.storage.eq(10)
        yield dut.source.ready.eq(1)
        yield dut.start.re.eq(1)
        yield
        yield dut.start.re.eq(0)
        while (yield dut.done.status):
            yield
        for _ in range(3000):
            if (yield dut.done.status):
                break
            yield
        assert (yield dut.done.status) == 1
        assert (yield dut.error.status) == 0
        assert (yield dut.progress.status) == 2

    run_simulation(dut, [consumer(), controller(), output_monitor(), clock_monitor()])
    assert command_counts == [2]
    assert len(output_cycles) == 2 * 512 // 8
    assert output_last == [0] * (len(output_last) - 1) + [1]
    assert all((b - a) >= 9 for a, b in zip(output_cycles, output_cycles[1:]))
