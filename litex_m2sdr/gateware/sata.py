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
# Keep RF stream captures in multi-sector SATA commands. This avoids the
# command/ACK overhead of issuing one write per 512-byte sector while keeping
# progress and interrupt latency bounded.
SATA_STREAM_BURST_SECTORS = 4096


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
    def __init__(self, port, data_width=64):
        self.port     = port
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(32, description="Number of SATA sectors to record.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when recording has completed.")
        self.error    = CSRStatus(description="Asserted when recording has failed.")
        self.progress = CSRStatus(32, description="Number of SATA sectors written in the current recording.")
        self.irq      = Signal()

        self.sink     = stream.Endpoint([("data", data_width)])

        # # #

        stream_bytes = data_width//8

        # Full-sector writes: require exact 512B chunks.
        assert (logical_sector_size % stream_bytes) == 0

        words_per_sector = _words_per_sector(port.dw)
        max_burst_sectors = min(SATA_STREAM_BURST_SECTORS, 0xffff)

        send_count        = Signal(32)
        burst_words       = Signal(32)
        burst_sectors     = Signal(16)
        remaining_sectors = Signal(32)
        crt_sec           = Signal(48)
        progress          = Signal(32)

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=data_width, nbits_to=port.dw)

        # Connect Stream to Converter.
        self.comb += self.sink.connect(conv.sink, keep={"valid", "ready", "data", "last"})
        self.comb += self.progress.status.eq(progress)

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(send_count,        0),
                NextValue(burst_words,       0),
                NextValue(burst_sectors,     0),
                NextValue(remaining_sectors, self.nsectors.storage),
                NextValue(crt_sec,           self.sector.storage),
                NextValue(progress,          0),
                *_clear_transfer_status(self.done, self.error),
                NextState("LOAD-BURST")
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("LOAD-BURST",
            NextValue(send_count, 0),
            If(remaining_sectors > max_burst_sectors,
                NextValue(burst_sectors, max_burst_sectors),
                NextValue(burst_words,   max_burst_sectors * words_per_sector),
            ).Else(
                NextValue(burst_sectors, remaining_sectors[:16]),
                NextValue(burst_words,   remaining_sectors * words_per_sector),
            ),
            NextState("SEND-CMD-AND-DATA")
        )
        fsm.act("SEND-CMD-AND-DATA",
            # Send one write command/data stream for the current burst. Gate
            # valid with converter output so the first SATA beat cannot use
            # stale data.
            port.sink.valid.eq(conv.source.valid),
            port.sink.last.eq(send_count == (burst_words - 1)),
            port.sink.write.eq(1),
            port.sink.sector.eq(crt_sec),
            port.sink.count.eq(burst_sectors),
            port.sink.data.eq(_sata_word(conv.source.data)),
            conv.source.ready.eq(port.sink.ready),
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
                    NextValue(progress, progress + burst_sectors),
                    If(remaining_sectors <= burst_sectors,
                        *_finish_transfer(self.done, self.irq),
                        NextState("IDLE")
                    ).Else(
                        NextValue(remaining_sectors, remaining_sectors - burst_sectors),
                        NextValue(crt_sec, crt_sec + burst_sectors),
                        NextState("LOAD-BURST")
                    )
                )
            )
        )


# SATA Sectors2Stream ------------------------------------------------------------------------------

class M2SDRLiteSATASectors2Stream(LiteXModule):
    """LiteSATA Sectors2Stream with port-width byte ordering.

    LiteSATA exposes a 32-bit port on the M2SDR design while the radio/PCIe
    stream is 64-bit. Reverse bytes at the SATA-port word width before
    up-conversion so the Stream2Sectors path is the exact inverse.
    """
    def __init__(self, port, data_width=64):
        self.port     = port
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(32, description="Number of SATA sectors to play.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when playback has completed.")
        self.error    = CSRStatus(description="Asserted when playback has failed.")
        self.irq      = Signal()

        self.source   = stream.Endpoint([("data", data_width)])

        # # #

        stream_bytes = data_width//8

        # Whole-sector streaming.
        assert (logical_sector_size % stream_bytes) == 0

        crt_sec  = Signal(48)
        last_sec = Signal(48)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], _words_per_sector(port.dw))

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=port.dw, nbits_to=data_width)

        # Connect Port to Sector Buffer. Reverse per SATA word, not per output
        # stream beat, so the down-conversion path writes the same words back.
        self.comb += [
            buf.sink.valid.eq(port.source.valid),
            buf.sink.last.eq(port.source.last),
            buf.sink.data.eq(_sata_word(port.source.data)),
            port.source.ready.eq(buf.sink.ready),
        ]

        # Connect Sector Buffer to Converter.
        self.comb += buf.source.connect(conv.sink)

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(crt_sec,  self.sector.storage),
                NextValue(last_sec, self.sector.storage + self.nsectors.storage - 1),
                *_clear_transfer_status(self.done, self.error),
                NextState("SEND-CMD")
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("SEND-CMD",
            # Send read command for 1 sector.
            port.sink.valid.eq(1),
            port.sink.last.eq(1),
            port.sink.read.eq(1),
            port.sink.sector.eq(crt_sec),
            port.sink.count.eq(1),
            If(port.sink.ready,
                NextState("RECEIVE-DATA-STREAM")
            )
        )
        fsm.act("RECEIVE-DATA-STREAM",
            # Connect Converter to Stream.
            self.source.valid.eq(conv.source.valid),
            self.source.data.eq(conv.source.data),

            # End-of-transfer framing:
            # Assert last only on the last word of the last sector.
            self.source.last.eq(conv.source.last & (crt_sec == last_sec)),

            conv.source.ready.eq(self.source.ready),

            If(self.source.valid & self.source.ready,
                If(conv.source.last,
                    If(crt_sec == last_sec,
                        *_finish_transfer(self.done, self.irq),
                        NextState("IDLE")
                    ).Else(
                        NextValue(crt_sec, crt_sec + 1),
                        NextState("SEND-CMD")
                    )
                )
            ),

            # Monitor errors.
            If(port.source.valid & port.source.ready & port.source.failed,
                *_fail_transfer(self.done, self.error, self.irq),
                NextState("IDLE")
            )
        )
