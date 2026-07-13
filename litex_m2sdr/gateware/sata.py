#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *
from litex.gen.common import reverse_bytes
from litex.soc.cores.dma import WishboneDMAReader, WishboneDMAWriter
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *
from litex.soc.interconnect import wishbone

from litesata.common import logical_sector_size


# Constants ----------------------------------------------------------------------------------------

SATA_HOST_BUFFER_BASE = 0x00020000
# 128KiB gives the Ethernet path 256-sector staging chunks while keeping the
# inferred RAM topology simple. 256KiB was tested and caused RAMB cascade DRC
# issues on the current Artix-7 target.
SATA_HOST_BUFFER_SIZE = 128 * 1024
# Use long SATA commands while buffering enough RF data to present the drive in
# contiguous bursts. A 384-sector preload / 512-sector FIFO is 192KiB / 256KiB
# respectively. The FIFO naturally alternates between refill and drain while a
# command remains active, avoiding both sparse word-by-word writes and the
# command/activate/ack overhead of issuing one command per small buffer.
#
# 0xffff is the largest non-zero ATA DMA EXT sector count represented directly
# by LiteSATA's 16-bit user port. Keeping the FIFO depth at a power of two avoids
# inefficient Artix-7 BRAM cascading.
SATA_STREAM_COMMAND_SECTORS = 0xffff
SATA_STREAM_PRELOAD_SECTORS = 384
SATA_STREAM_LOW_WATERMARK_SECTORS = 64
SATA_STREAM_FIFO_SECTORS    = 512


# Helpers ------------------------------------------------------------------------------------------

def _clear_transfer_status(done, error):
    return [
        NextValue(done.status,  0),
        NextValue(error.status, 0),
    ]


def _finish_transfer(done, irq):
    return [
        irq.eq(1),
        NextValue(done.status, 1),
    ]


def _fail_transfer(done, error, irq):
    return [
        irq.eq(1),
        NextValue(done.status,  1),
        NextValue(error.status, 1),
    ]


def _words_per_sector(data_width):
    return logical_sector_size//(data_width//8)


def _sata_word(data):
    # LiteSATA presents drive words in the opposite byte order from the local
    # little-endian host/radio streams.
    return reverse_bytes(data)


class SATAStreamTap(LiteXModule):
    """Losslessly duplicate a primary stream while the tap is enabled.

    A word is presented to each output only when the other output can accept
    it, so neither consumer can advance independently and observe duplicates.
    With the tap disabled, the primary path is a normal transparent link.
    """
    def __init__(self, layout):
        self.sink    = stream.Endpoint(layout)
        self.source  = stream.Endpoint(layout)
        self.tap     = stream.Endpoint(layout)
        self.enable  = Signal()

        # # #

        self.primary_fifo = primary_fifo = stream.SyncFIFO(layout, depth=4, buffered=True)
        self.tap_fifo = tap_fifo = ResetInserter()(stream.SyncFIFO(layout, depth=4, buffered=True))

        self.comb += [
            self.sink.connect(primary_fifo.sink, omit={"valid", "ready"}),
            self.sink.connect(tap_fifo.sink,     omit={"valid", "ready"}),
            primary_fifo.source.connect(self.source),
            tap_fifo.source.connect(self.tap),

            # Both queues accept a tapped word in the same cycle.  Their
            # registered occupancy breaks the ready/valid path between the
            # PCIe and SATA consumers while sustaining one word per cycle.
            primary_fifo.sink.valid.eq(
                self.sink.valid & (~self.enable | tap_fifo.sink.ready)),
            tap_fifo.sink.valid.eq(
                self.sink.valid & self.enable & primary_fifo.sink.ready),
            self.sink.ready.eq(
                primary_fifo.sink.ready & (~self.enable | tap_fifo.sink.ready)),
            tap_fifo.reset.eq(~self.enable),
        ]


# SATA Host Buffer ---------------------------------------------------------------------------------

class SATAHostBuffer(LiteXModule):
    """Host-visible scratch RAM used to stage SATA transfers.

    The host port is mapped on the SoC Wishbone bus. The optional DMA port lets
    the SATA DMA engines reach the same RAM directly, or through a small router
    when PCIe host memory is also visible.
    """
    def __init__(self, size=SATA_HOST_BUFFER_SIZE, with_dma_port=True):
        self.host_bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        if with_dma_port:
            self.dma_bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")

        # # #

        data_bytes = self.host_bus.data_width//8
        assert size > 0
        assert (size % data_bytes) == 0
        assert (size % logical_sector_size) == 0
        assert (size & (size - 1)) == 0

        depth = size // data_bytes
        mem = Memory(32, depth, name="sata_host_buffer")
        self.specials += mem

        self._add_port(mem, self.host_bus)
        if with_dma_port:
            self._add_port(mem, self.dma_bus)

    def _add_port(self, mem, bus):
        port = mem.get_port(write_capable=True, we_granularity=8, mode=WRITE_FIRST)
        self.specials += port
        self.comb += [
            port.adr.eq(bus.adr[:len(port.adr)]),
            port.dat_w.eq(bus.dat_w),
            bus.dat_r.eq(port.dat_r),
        ]
        self.comb += [
            port.we[i].eq(bus.cyc & bus.stb & bus.we & bus.sel[i])
            for i in range(4)
        ]
        # Single-cycle registered ACK, matching the usual LiteX SRAM behavior.
        self.sync += [
            bus.ack.eq(0),
            If(bus.cyc & bus.stb & ~bus.ack,
                bus.ack.eq(1)
            )
        ]


# SATA DMA Memory Router ---------------------------------------------------------------------------

class SATADMAMemoryRouter(LiteXModule):
    """Route SATA DMA accesses between local staging RAM and host memory."""
    def __init__(self, local_bus, remote_bus, local_origin, local_size):
        if local_bus.addressing != remote_bus.addressing:
            raise ValueError("SATA DMA local/remote bus addressing mismatch.")
        if local_bus.data_width != remote_bus.data_width:
            raise ValueError("SATA DMA local/remote bus data width mismatch.")
        bus_bytes = local_bus.data_width//8
        if local_origin % bus_bytes:
            raise ValueError("SATA DMA local origin must be bus-word aligned.")
        if local_size % bus_bytes:
            raise ValueError("SATA DMA local size must be bus-word aligned.")

        self.bus = bus = wishbone.Interface(
            data_width    = local_bus.data_width,
            address_width = local_bus.address_width,
            addressing    = local_bus.addressing,
        )

        # # #

        word_shift = log2_int(local_bus.data_width//8) if bus.addressing == "word" else 0
        bus_addr = Signal(bus.address_width)
        local_sel = Signal()
        local_limit = local_origin + local_size
        self.comb += [
            bus_addr.eq(bus.adr << word_shift),
            local_sel.eq((bus_addr >= local_origin) & (bus_addr < local_limit)),
        ]

        local_origin_words = local_origin >> word_shift

        self.comb += [
            local_bus.adr.eq(bus.adr - local_origin_words),
            remote_bus.adr.eq(bus.adr),
        ]
        for target_bus, target_sel in [(local_bus, local_sel), (remote_bus, ~local_sel)]:
            self.comb += [
                target_bus.dat_w.eq(bus.dat_w),
                target_bus.sel.eq(bus.sel),
                target_bus.we.eq(bus.we),
                target_bus.cti.eq(bus.cti),
                target_bus.bte.eq(bus.bte),
                target_bus.stb.eq(bus.stb),
                target_bus.cyc.eq(bus.cyc & target_sel),
            ]

        self.comb += [
            bus.ack.eq((local_bus.ack & local_sel) | (remote_bus.ack & ~local_sel)),
            bus.err.eq((local_bus.err & local_sel) | (remote_bus.err & ~local_sel)),
            bus.dat_r.eq(Mux(local_sel, local_bus.dat_r, remote_bus.dat_r)),
        ]


# SATA Sector2Mem DMA ------------------------------------------------------------------------------

class M2SDRLiteSATASector2MemDMA(LiteXModule):
    """LiteSATA Sector2Mem DMA with contiguous multi-sector host-buffer reads."""
    def __init__(self, port, bus, endianness="little"):
        self.port     = port
        self.bus      = bus
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(16, description="Number of SATA sectors to transfer.")
        self.base     = CSRStorage(64, description="Destination Wishbone base address.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when the transfer has completed.")
        self.error    = CSRStatus(description="Asserted when the transfer has failed.")
        self.irq      = Signal()

        # # #

        dma_bytes   = bus.data_width//8
        count       = Signal(32)
        total_words = Signal(32)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], _words_per_sector(port.dw))

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=port.dw, nbits_to=bus.data_width)

        sink_read_data = Signal()
        source_ready   = Signal()

        # Connect read-data packets to the Sector Buffer. The final command
        # status response is consumed separately so it cannot be stored as data.
        self.comb += [
            buf.sink.valid.eq(sink_read_data & port.source.valid & port.source.read & ~port.source.end),
            buf.sink.last.eq(port.source.last),
            buf.sink.data.eq(port.source.data),
            port.source.ready.eq(source_ready),
        ]

        # Connect Sector Buffer to Converter.
        self.comb += buf.source.connect(conv.sink)

        # DMA.
        self.dma = dma = WishboneDMAWriter(bus, with_csr=False, endianness=endianness)

        # Fence bus.
        # PCIe memory writes are posted. When this DMA targets host memory through
        # LitePCIe, the final Wishbone write ACK only means the MWr TLP was
        # accepted by the endpoint. A following MRd completion provides a
        # low-cost ordering fence before software observes done.
        self.fence_bus = fence_bus = wishbone.Interface(
            data_width    = bus.data_width,
            address_width = bus.address_width,
            bursting      = bus.bursting,
            addressing    = bus.addressing,
            mode          = "r",
        )
        self.fence_dma = fence_dma = WishboneDMAReader(
            fence_bus,
            with_csr   = False,
            endianness = endianness,
        )

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(count,       0),
                NextValue(total_words, self.nsectors.storage * _words_per_sector(bus.data_width)),
                *_clear_transfer_status(self.done, self.error),
                NextState("SEND-CMD")
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("SEND-CMD",
            # Send one read command for the full requested host-buffer chunk.
            port.sink.valid.eq(1),
            port.sink.last.eq(1),
            port.sink.read.eq(1),
            port.sink.sector.eq(self.sector.storage),
            port.sink.count.eq(self.nsectors.storage),
            If(port.sink.ready,
                NextState("RECEIVE-DATA-DMA")
            )
        )
        fsm.act("RECEIVE-DATA-DMA",
            sink_read_data.eq(1),
            source_ready.eq(Mux(port.source.read & ~port.source.end, buf.sink.ready, 0)),

            # Connect Converter to DMA.
            dma.sink.valid.eq(conv.source.valid),
            dma.sink.last.eq(count == (total_words - 1)),
            dma.sink.address.eq(self.base.storage[log2_int(dma_bytes):] + count),
            dma.sink.data.eq(_sata_word(conv.source.data)),
            conv.source.ready.eq(dma.sink.ready),
            If(dma.sink.valid & dma.sink.ready,
                If(dma.sink.last,
                    NextState("WAIT-RESPONSE")
                ).Else(
                    NextValue(count, count + 1)
                )
            ),

            # Monitor errors.
            If(port.source.valid & port.source.ready & port.source.failed,
                *_fail_transfer(self.done, self.error, self.irq),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-RESPONSE",
            source_ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.read & port.source.end,
                If(port.source.failed,
                    *_fail_transfer(self.done, self.error, self.irq),
                    NextState("IDLE")
                ).Else(
                    NextState("FLUSH-HOST-WRITES")
                ),
            ),
            If(port.source.valid & port.source.ready & port.source.failed,
                *_fail_transfer(self.done, self.error, self.irq),
                NextState("IDLE"),
            )
        )
        fsm.act("FLUSH-HOST-WRITES",
            fence_dma.sink.valid.eq(1),
            fence_dma.sink.last.eq(1),
            fence_dma.sink.address.eq(self.base.storage[log2_int(dma_bytes):]),
            If(fence_dma.sink.ready,
                NextState("WAIT-FLUSH")
            )
        )
        fsm.act("WAIT-FLUSH",
            fence_dma.source.ready.eq(1),
            If(fence_dma.source.valid,
                *_finish_transfer(self.done, self.irq),
                NextState("IDLE")
            )
        )


# SATA Mem2Sector DMA ------------------------------------------------------------------------------

class M2SDRLiteSATAMem2SectorDMA(LiteXModule):
    """LiteSATA Mem2Sector DMA with valid-gated SATA sink handshakes."""
    def __init__(self, bus, port, endianness="little"):
        self.bus      = bus
        self.port     = port
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(16, description="Number of SATA sectors to transfer.")
        self.base     = CSRStorage(64, description="Source Wishbone base address.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when the transfer has completed.")
        self.error    = CSRStatus(description="Asserted when the transfer has failed.")
        self.irq      = Signal()

        # # #

        dma_bytes        = bus.data_width//8
        read_count       = Signal(32)
        send_count       = Signal(32)
        total_dma_words  = Signal(32)
        total_port_words = Signal(32)
        dma_active       = Signal()

        # DMA.
        self.dma = dma = WishboneDMAReader(bus, with_csr=False, endianness=endianness)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], _words_per_sector(bus.data_width))

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=bus.data_width, nbits_to=port.dw)

        # Connect DMA to Sector Buffer.
        self.comb += dma.source.connect(buf.sink)

        # Connect Sector Buffer to Converter.
        self.comb += buf.source.connect(conv.sink)

        self.comb += [
            total_dma_words.eq(self.nsectors.storage * _words_per_sector(bus.data_width)),
            total_port_words.eq(self.nsectors.storage * _words_per_sector(port.dw)),
        ]

        self.comb += [
            dma.sink.valid.eq(dma_active & (read_count != total_dma_words)),
            dma.sink.last.eq(read_count == (total_dma_words - 1)),
            dma.sink.address.eq(self.base.storage[log2_int(dma_bytes):] + read_count),
        ]

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(read_count, 0),
                NextValue(send_count, 0),
                *_clear_transfer_status(self.done, self.error),
                NextState("READ-DATA-DMA")
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("READ-DATA-DMA",
            # Start filling the DMA reader FIFO so the SATA command can be
            # followed immediately by data once the drive activates DMA.
            dma_active.eq(1),
            If(dma.sink.valid & dma.sink.ready,
                NextValue(read_count, read_count + 1),
            ),
            If(conv.source.valid,
                NextState("SEND-CMD-AND-DATA")
            )
        )
        fsm.act("SEND-CMD-AND-DATA",
            # Send one write command/data stream for the full request. Gate valid with the
            # converter output so the first SATA beat cannot use stale data.
            dma_active.eq(1),
            port.sink.valid.eq(conv.source.valid),
            port.sink.last.eq(send_count == (total_port_words - 1)),
            port.sink.write.eq(1),
            port.sink.sector.eq(self.sector.storage),
            port.sink.count.eq(self.nsectors.storage),
            port.sink.data.eq(_sata_word(conv.source.data)),
            If(dma.sink.valid & dma.sink.ready,
                NextValue(read_count, read_count + 1),
            ),
            If(port.sink.valid & port.sink.ready,
                conv.source.ready.eq(1),
                NextValue(send_count, send_count + 1),
                If(port.sink.last,
                    NextState("WAIT-ACK")
                )
            ),

            # Monitor errors.
            port.source.ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.failed,
                *_fail_transfer(self.done, self.error, self.irq),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-ACK",
            port.source.ready.eq(1),
            If(port.source.valid,
                If(port.source.failed,
                    *_fail_transfer(self.done, self.error, self.irq),
                    NextState("IDLE")
                ).Else(
                    *_finish_transfer(self.done, self.irq),
                    NextState("IDLE")
                )
            )
        )


# SATA Stream2Sectors ------------------------------------------------------------------------------

class M2SDRLiteSATAStream2Sectors(LiteXModule):
    """LiteSATA Stream2Sectors with contiguous multi-sector SATA writes."""
    def __init__(self, port, data_width=64,
        burst_sectors=SATA_STREAM_COMMAND_SECTORS,
        preload_sectors=SATA_STREAM_PRELOAD_SECTORS,
        low_watermark_sectors=SATA_STREAM_LOW_WATERMARK_SECTORS,
        fifo_sectors=SATA_STREAM_FIFO_SECTORS):
        self.port     = port
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(32, description="Number of SATA sectors to record.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when recording has completed.")
        self.error    = CSRStatus(description="Asserted when recording has failed.")
        self.progress = CSRStatus(32, description="Number of SATA sectors written in the current recording.")
        self.tap      = CSRStorage(description=
            "Duplicate the selected host RX stream into this recorder while preserving the host path.")
        self.irq      = Signal()

        self.sink     = stream.Endpoint([("data", data_width)])

        # # #

        stream_bytes = data_width//8

        # Full-sector writes: require exact 512B chunks.
        assert (logical_sector_size % stream_bytes) == 0

        words_per_sector        = _words_per_sector(port.dw)
        stream_words_per_sector = logical_sector_size//stream_bytes
        max_command_sectors     = min(burst_sectors, 0xffff)
        max_preload_sectors     = min(preload_sectors, fifo_sectors - 1,
                                      max_command_sectors)
        low_watermark_sectors   = min(low_watermark_sectors,
                                      max_preload_sectors - 1)
        fifo_depth              = fifo_sectors * stream_words_per_sector
        low_stream_words        = low_watermark_sectors * stream_words_per_sector
        low_port_words          = low_watermark_sectors * words_per_sector

        assert max_command_sectors > 0
        assert max_preload_sectors > 0
        assert low_watermark_sectors > 0
        assert fifo_sectors > max_preload_sectors

        # Accumulate a useful reservoir before presenting a long command to
        # LiteSATA. The FIFO continues accepting RF words throughout the
        # command. When SATA drains it faster than RF can refill it, valid is
        # temporarily deasserted. High/low-watermark hysteresis prevents the
        # FIFO from draining completely and degenerating back into sparse
        # word-by-word SATA traffic. The same command resumes after the FIFO
        # refills, avoiding command overhead for every reservoir-sized chunk.
        self.input_fifo = input_fifo = stream.SyncFIFO(
            [("data", data_width)], depth=fifo_depth, buffered=True)

        send_count        = Signal(32)
        command_words       = Signal(32)
        preload_stream_words = Signal(32)
        command_sectors     = Signal(16)
        remaining_sectors = Signal(32)
        crt_sec           = Signal(48)
        progress          = Signal(32)
        drain_enabled     = Signal()

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=data_width, nbits_to=port.dw)

        # Connect Stream through the burst reservoir to the converter.
        self.comb += [
            self.sink.connect(input_fifo.sink, keep={"valid", "ready", "data", "last"}),
            input_fifo.source.connect(conv.sink, keep={"valid", "ready", "data", "last"}),
        ]
        self.comb += self.progress.status.eq(progress)

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(send_count,        0),
                NextValue(command_words,       0),
                NextValue(preload_stream_words, 0),
                NextValue(command_sectors,     0),
                NextValue(remaining_sectors, self.nsectors.storage),
                NextValue(crt_sec,           self.sector.storage),
                NextValue(progress,          0),
                NextValue(drain_enabled,     0),
                *_clear_transfer_status(self.done, self.error),
                NextState("LOAD-BURST")
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("LOAD-BURST",
            NextValue(send_count, 0),
            If(remaining_sectors > max_command_sectors,
                NextValue(command_sectors, max_command_sectors),
                NextValue(command_words,   max_command_sectors * words_per_sector),
                NextValue(preload_stream_words,
                    max_preload_sectors * stream_words_per_sector),
            ).Else(
                NextValue(command_sectors, remaining_sectors[:16]),
                NextValue(command_words,   remaining_sectors * words_per_sector),
                If(remaining_sectors > max_preload_sectors,
                    NextValue(preload_stream_words,
                        max_preload_sectors * stream_words_per_sector),
                ).Else(
                    NextValue(preload_stream_words,
                        remaining_sectors * stream_words_per_sector),
                )
            ),
            NextState("WAIT-BURST-DATA")
        )
        fsm.act("WAIT-BURST-DATA",
            If(input_fifo.level >= preload_stream_words,
                NextValue(drain_enabled, 1),
                NextState("SEND-CMD-AND-DATA")
            )
        )
        fsm.act("SEND-CMD-AND-DATA",
            # Send one write command/data stream for the current burst. Gate
            # valid with converter output so the first SATA beat cannot use
            # stale data.
            port.sink.valid.eq(conv.source.valid & drain_enabled),
            port.sink.last.eq(send_count == (command_words - 1)),
            port.sink.write.eq(1),
            port.sink.sector.eq(crt_sec),
            port.sink.count.eq(command_sectors),
            port.sink.data.eq(_sata_word(conv.source.data)),
            conv.source.ready.eq(port.sink.ready & drain_enabled),
            # Send only contiguous reservoir bursts. Keep draining the last
            # low-watermark portion of a command so a short final command
            # cannot wait for data that belongs to the next command.
            If(drain_enabled,
                If((input_fifo.level <= low_stream_words) &
                   (command_sectors > low_watermark_sectors) &
                   (send_count < (command_words - low_port_words)),
                    NextValue(drain_enabled, 0)
                )
            ).Elif(input_fifo.level >= preload_stream_words,
                NextValue(drain_enabled, 1)
            ),
            If(port.sink.valid & port.sink.ready,
                NextValue(send_count, send_count + 1),
                If(port.sink.last,
                    NextState("WAIT-ACK")
                )
            ),

            # Monitor errors.
            port.source.ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.failed,
                *_fail_transfer(self.done, self.error, self.irq),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-ACK",
            port.source.ready.eq(1),
            If(port.source.valid,
                If(port.source.failed,
                    *_fail_transfer(self.done, self.error, self.irq),
                    NextState("IDLE")
                ).Else(
                    NextValue(progress, progress + command_sectors),
                    If(remaining_sectors <= command_sectors,
                        *_finish_transfer(self.done, self.irq),
                        NextState("IDLE")
                    ).Else(
                        NextValue(remaining_sectors, remaining_sectors - command_sectors),
                        NextValue(crt_sec, crt_sec + command_sectors),
                        NextState("LOAD-BURST")
                    )
                )
            )
        )


# SATA Sectors2Stream ------------------------------------------------------------------------------

class M2SDRLiteSATASectors2Stream(LiteXModule):
    """Buffered, paced LiteSATA Sectors2Stream.

    LiteSATA exposes a 32-bit port on the M2SDR design while the radio/PCIe
    stream is 64-bit. Reverse bytes at the SATA-port word width before
    up-conversion so the Stream2Sectors path is the exact inverse.

    Reads are issued as long multi-sector commands through a bounded FIFO.
    Ready/valid backpressure pauses the drive whenever that FIFO fills, while
    the optional pacer releases 64-bit words at the recorded RF byte rate.
    """
    def __init__(self, port, data_width=64, sys_clk_freq=int(100e6),
        burst_sectors=SATA_STREAM_COMMAND_SECTORS,
        fifo_sectors=SATA_STREAM_FIFO_SECTORS):
        self.port     = port
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(32, description="Number of SATA sectors to play.")
        self.pace     = CSRStorage(32, description=
            "Replay rate in 64-bit stream words/s. Zero disables pacing.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when playback has completed.")
        self.error    = CSRStatus(description="Asserted when playback has failed.")
        self.progress = CSRStatus(32, description=
            "Number of SATA sectors read in the current playback.")
        self.irq      = Signal()

        self.source   = stream.Endpoint([("data", data_width)])

        # # #

        stream_bytes = data_width//8

        # Whole-sector streaming.
        assert (logical_sector_size % stream_bytes) == 0

        stream_words_per_sector = logical_sector_size//stream_bytes
        port_words_per_sector   = _words_per_sector(port.dw)
        max_command_sectors     = min(burst_sectors, 0xffff)
        fifo_depth              = fifo_sectors * stream_words_per_sector

        assert data_width == 64
        assert max_command_sectors > 0
        assert fifo_sectors > 0

        crt_sec            = Signal(48)
        remaining_sectors  = Signal(32)
        command_sectors    = Signal(16)
        command_port_words = Signal(32)
        receive_count      = Signal(32)
        progress           = Signal(32)
        final_output_seen  = Signal()

        # FIFO after width conversion lets SATA fetch the next burst while the
        # current one is being released at the RF stream rate.
        self.output_fifo = output_fifo = stream.SyncFIFO(
            [("data", data_width)], depth=fifo_depth, buffered=True)

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=port.dw, nbits_to=data_width)

        # Connect the read response to the converter only while a command is
        # active. Reverse per SATA word, not per output stream beat, so the
        # down-conversion path writes the same words back.
        self.comb += [
            conv.sink.valid.eq(0),
            conv.sink.last.eq(0),
            conv.sink.data.eq(_sata_word(port.source.data)),
            port.source.ready.eq(0),

            output_fifo.sink.valid.eq(conv.source.valid),
            output_fifo.sink.last.eq(
                conv.source.last & (remaining_sectors <= command_sectors)),
            output_fifo.sink.data.eq(conv.source.data),
            conv.source.ready.eq(output_fifo.sink.ready),
        ]

        self.comb += self.progress.status.eq(progress)

        # Fractional word-rate pacer. A single pending credit is retained when
        # the downstream host is backpressured; credits are deliberately not
        # accumulated into a burst.
        pace_accumulator = Signal(max=max(2, int(sys_clk_freq)))
        pace_sum         = Signal(33)
        pace_credit      = Signal()
        pace_full_rate   = Signal()
        pace_tick        = Signal()
        pace_allow       = Signal()
        output_fire      = Signal()

        self.comb += [
            pace_sum.eq(pace_accumulator + self.pace.storage),
            pace_full_rate.eq(
                (self.pace.storage == 0) | (self.pace.storage >= int(sys_clk_freq))),
            pace_tick.eq(pace_sum >= int(sys_clk_freq)),
            pace_allow.eq(pace_full_rate | pace_credit | pace_tick),

            self.source.valid.eq(output_fifo.source.valid & pace_allow),
            self.source.last.eq(output_fifo.source.last),
            self.source.data.eq(output_fifo.source.data),
            output_fifo.source.ready.eq(self.source.ready & pace_allow),
            output_fire.eq(self.source.valid & self.source.ready),
        ]

        self.sync += [
            If(self.start.re,
                pace_accumulator.eq(0),
                pace_credit.eq(0),
                final_output_seen.eq(0),
            ).Elif(pace_full_rate,
                pace_accumulator.eq(0),
                pace_credit.eq(0),
            ).Elif(pace_credit,
                If(output_fire,
                    pace_credit.eq(0),
                )
            ).Else(
                If(pace_tick,
                    pace_accumulator.eq(pace_sum - int(sys_clk_freq)),
                    If(~output_fire,
                        pace_credit.eq(1),
                    )
                ).Else(
                    pace_accumulator.eq(pace_sum),
                )
            ),
            If(output_fire & self.source.last,
                final_output_seen.eq(1),
            )
        ]

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(crt_sec,           self.sector.storage),
                NextValue(remaining_sectors, self.nsectors.storage),
                NextValue(command_sectors,   0),
                NextValue(command_port_words, 0),
                NextValue(receive_count,    0),
                NextValue(progress,          0),
                *_clear_transfer_status(self.done, self.error),
                NextState("LOAD-BURST")
            )
        )
        fsm.act("LOAD-BURST",
            If(remaining_sectors > max_command_sectors,
                NextValue(command_sectors, max_command_sectors),
                NextValue(command_port_words,
                    max_command_sectors * port_words_per_sector),
            ).Else(
                NextValue(command_sectors, remaining_sectors[:16]),
                NextValue(command_port_words,
                    remaining_sectors * port_words_per_sector),
            ),
            NextState("WAIT-FIFO-SPACE")
        )
        fsm.act("WAIT-FIFO-SPACE",
            # A command can be much larger than the FIFO: LiteSATA propagates
            # ready/valid backpressure to the drive while paced output makes
            # room. Start with half the reservoir free so useful read data can
            # arrive before the first HOLD/backpressure interval.
            If(output_fifo.level <= (fifo_depth >> 1),
                NextState("SEND-CMD")
            )
        )
        fsm.act("SEND-CMD",
            # Send one command for the complete burst.
            port.sink.valid.eq(1),
            port.sink.last.eq(1),
            port.sink.read.eq(1),
            port.sink.sector.eq(crt_sec),
            port.sink.count.eq(command_sectors),
            If(port.sink.ready,
                NextValue(receive_count, 0),
                NextState("RECEIVE-DATA-STREAM")
            )
        )
        fsm.act("RECEIVE-DATA-STREAM",
            # A multi-sector read can contain multiple DATA FIS packets, each
            # with last asserted. Count payload words and wait for the separate
            # end response instead of treating a packet boundary as command
            # completion.
            conv.sink.valid.eq(port.source.valid & port.source.read & ~port.source.end),
            conv.sink.last.eq(receive_count == (command_port_words - 1)),
            port.source.ready.eq(
                conv.sink.ready & port.source.read & ~port.source.end),
            If(port.source.valid & port.source.ready,
                If(port.source.failed,
                    *_fail_transfer(self.done, self.error, self.irq),
                    NextState("IDLE")
                ).Elif(receive_count == (command_port_words - 1),
                    NextState("WAIT-RESPONSE")
                ).Else(
                    NextValue(receive_count, receive_count + 1)
                )
            )
        )
        fsm.act("WAIT-RESPONSE",
            port.source.ready.eq(1),
            If(port.source.valid & port.source.ready,
                If(port.source.failed,
                    *_fail_transfer(self.done, self.error, self.irq),
                    NextState("IDLE")
                ).Elif(port.source.read & port.source.end,
                    NextValue(progress, progress + command_sectors),
                    If(remaining_sectors <= command_sectors,
                        NextState("DRAIN-FINAL")
                    ).Else(
                        NextValue(remaining_sectors, remaining_sectors - command_sectors),
                        NextValue(crt_sec, crt_sec + command_sectors),
                        NextState("LOAD-BURST")
                    )
                )
            )
        )
        fsm.act("DRAIN-FINAL",
            If(final_output_seen,
                *_finish_transfer(self.done, self.irq),
                NextState("IDLE")
            )
        )
