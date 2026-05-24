# SPDX-License-Identifier: BSD-2-Clause
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>

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
SATA_HOST_BUFFER_SIZE = 64 * 1024


# SATA Host Buffer ---------------------------------------------------------------------------------

class SATAHostBuffer(LiteXModule):
    def __init__(self, size=SATA_HOST_BUFFER_SIZE, with_dma_port=True):
        self.host_bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        if with_dma_port:
            self.dma_bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")

        # # #

        depth = size // 4
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
        self.sync += [
            bus.ack.eq(0),
            If(bus.cyc & bus.stb & ~bus.ack,
                bus.ack.eq(1)
            )
        ]


# SATA DMA Memory Router ---------------------------------------------------------------------------

class SATADMAMemoryRouter(LiteXModule):
    def __init__(self, local_bus, remote_bus, local_origin, local_size):
        if local_bus.addressing != remote_bus.addressing:
            raise ValueError("SATA DMA local/remote bus addressing mismatch.")
        if local_bus.data_width != remote_bus.data_width:
            raise ValueError("SATA DMA local/remote bus data width mismatch.")

        self.bus = bus = wishbone.Interface(
            data_width    = local_bus.data_width,
            address_width = local_bus.address_width,
            addressing    = local_bus.addressing,
        )

        # # #

        word_shift = log2_int(local_bus.data_width//8) if bus.addressing == "word" else 0
        bus_addr = Signal(bus.address_width)
        local_sel = Signal()
        self.comb += [
            bus_addr.eq(bus.adr << word_shift),
            local_sel.eq((bus_addr >= local_origin) & (bus_addr < (local_origin + local_size))),
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

        port_bytes = port.dw//8
        dma_bytes  = bus.data_width//8
        count       = Signal(32)
        total_words = Signal(32)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], logical_sector_size//port_bytes)

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
                NextValue(count,             0),
                NextValue(total_words,       self.nsectors.storage * (logical_sector_size//dma_bytes)),
                NextValue(self.done.status,  0),
                NextValue(self.error.status, 0),
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
            dma.sink.data.eq(reverse_bytes(conv.source.data)),
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
                self.irq.eq(1),
                NextValue(self.done.status, 1),
                NextValue(self.error.status, 1),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-RESPONSE",
            source_ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.read & port.source.end,
                If(port.source.failed,
                    NextValue(self.error.status, 1),
                    self.irq.eq(1),
                    NextValue(self.done.status, 1),
                    NextState("IDLE")
                ).Else(
                    NextState("FLUSH-HOST-WRITES")
                ),
            ),
            If(port.source.valid & port.source.ready & port.source.failed,
                self.irq.eq(1),
                NextValue(self.done.status, 1),
                NextValue(self.error.status, 1),
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
                self.irq.eq(1),
                NextValue(self.done.status, 1),
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
        port_bytes       = port.dw//8
        read_count       = Signal(32)
        send_count       = Signal(32)
        total_dma_words  = Signal(32)
        total_port_words = Signal(32)
        dma_active       = Signal()

        # DMA.
        self.dma = dma = WishboneDMAReader(bus, with_csr=False, endianness=endianness)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], logical_sector_size//dma_bytes)

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=bus.data_width, nbits_to=port.dw)

        # Connect DMA to Sector Buffer.
        self.comb += dma.source.connect(buf.sink)

        # Connect Sector Buffer to Converter.
        self.comb += buf.source.connect(conv.sink)

        self.comb += [
            total_dma_words.eq(self.nsectors.storage * (logical_sector_size//dma_bytes)),
            total_port_words.eq(self.nsectors.storage * (logical_sector_size//port_bytes)),
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
                NextValue(read_count,        0),
                NextValue(send_count,        0),
                NextValue(self.done.status,  0),
                NextValue(self.error.status, 0),
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
            port.sink.data.eq(reverse_bytes(conv.source.data)),
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
                self.irq.eq(1),
                NextValue(self.done.status, 1),
                NextValue(self.error.status, 1),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-ACK",
            port.source.ready.eq(1),
            If(port.source.valid,
                If(port.source.failed,
                    NextValue(self.error.status, 1),
                    NextValue(self.done.status, 1),
                    self.irq.eq(1),
                    NextState("IDLE")
                ).Else(
                    self.irq.eq(1),
                    NextValue(self.done.status, 1),
                    NextState("IDLE")
                )
            )
        )


# SATA Stream2Sectors ------------------------------------------------------------------------------

class M2SDRLiteSATAStream2Sectors(LiteXModule):
    """LiteSATA Stream2Sectors with staged, valid-gated SATA writes."""
    def __init__(self, port, data_width=64):
        self.port     = port
        self.sector   = CSRStorage(48, description="First SATA sector.")
        self.nsectors = CSRStorage(32, description="Number of SATA sectors to record.")
        self.start    = CSR()
        self.done     = CSRStatus(reset=1, description="Asserted when recording has completed.")
        self.error    = CSRStatus(description="Asserted when recording has failed.")
        self.irq      = Signal()

        self.sink     = stream.Endpoint([("data", data_width)])

        # # #

        port_bytes   = port.dw//8
        stream_bytes = data_width//8

        # Full-sector writes: require exact 512B chunks.
        assert (logical_sector_size % stream_bytes) == 0

        words_per_sector = logical_sector_size//port_bytes

        count   = Signal(max=words_per_sector)
        crt_sec = Signal(48)

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=data_width, nbits_to=port.dw)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], words_per_sector)

        # Connect Stream to Converter.
        self.comb += self.sink.connect(conv.sink, keep={"valid", "ready", "data", "last"})

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(count,             0),
                NextValue(crt_sec,           self.sector.storage),
                NextValue(self.done.status,  0),
                NextValue(self.error.status, 0),
                NextState("FILL-SECTOR")
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("FILL-SECTOR",
            # Stage exactly one sector before issuing the SATA write command.
            # This decouples SATA reads feeding the stream from SATA writes.
            buf.sink.valid.eq(conv.source.valid),
            buf.sink.data.eq(conv.source.data),
            conv.source.ready.eq(buf.sink.ready),
            If(buf.sink.valid & buf.sink.ready,
                NextValue(count, count + 1),
                If(count == (words_per_sector - 1),
                    NextValue(count, 0),
                    NextState("SEND-CMD-AND-DATA")
                )
            )
        )
        fsm.act("SEND-CMD-AND-DATA",
            # Send write command/data for 1 sector. Gate valid with buffered
            # data so SATA never samples stale stream data.
            port.sink.valid.eq(buf.source.valid),
            port.sink.last.eq(count == (words_per_sector - 1)),
            port.sink.write.eq(1),
            port.sink.sector.eq(crt_sec),
            port.sink.count.eq(1),
            port.sink.data.eq(reverse_bytes(buf.source.data)),
            buf.source.ready.eq(port.sink.ready),
            If(port.sink.valid & port.sink.ready,
                NextValue(count, count + 1),
                If(port.sink.last,
                    NextState("WAIT-ACK")
                )
            ),

            # Monitor errors.
            port.source.ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.failed,
                self.irq.eq(1),
                NextValue(self.done.status, 1),
                NextValue(self.error.status, 1),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-ACK",
            port.source.ready.eq(1),
            If(port.source.valid,
                If(port.source.failed,
                    NextValue(self.error.status, 1),
                    NextValue(self.done.status, 1),
                    self.irq.eq(1),
                    NextState("IDLE")
                ).Elif(crt_sec == (self.sector.storage + self.nsectors.storage - 1),
                    self.irq.eq(1),
                    NextValue(self.done.status, 1),
                    NextState("IDLE")
                ).Else(
                    NextValue(count,   0),
                    NextValue(crt_sec, crt_sec + 1),
                    NextState("FILL-SECTOR")
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

        port_bytes   = port.dw//8
        stream_bytes = data_width//8

        # Whole-sector streaming.
        assert (logical_sector_size % stream_bytes) == 0

        crt_sec  = Signal(48)
        last_sec = Signal(48)

        # Sector buffer.
        self.buf = buf = stream.SyncFIFO([("data", port.dw)], logical_sector_size//port_bytes)

        # Converter.
        self.conv = conv = stream.Converter(nbits_from=port.dw, nbits_to=data_width)

        # Connect Port to Sector Buffer. Reverse per SATA word, not per output
        # stream beat, so the down-conversion path writes the same words back.
        self.comb += [
            buf.sink.valid.eq(port.source.valid),
            buf.sink.last.eq(port.source.last),
            buf.sink.data.eq(reverse_bytes(port.source.data)),
            port.source.ready.eq(buf.sink.ready),
        ]

        # Connect Sector Buffer to Converter.
        self.comb += buf.source.connect(conv.sink)

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(crt_sec,           self.sector.storage),
                NextValue(last_sec,          self.sector.storage + self.nsectors.storage - 1),
                NextValue(self.done.status,  0),
                NextValue(self.error.status, 0),
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
                        self.irq.eq(1),
                        NextValue(self.done.status, 1),
                        NextState("IDLE")
                    ).Else(
                        NextValue(crt_sec, crt_sec + 1),
                        NextState("SEND-CMD")
                    )
                )
            ),

            # Monitor errors.
            If(port.source.valid & port.source.ready & port.source.failed,
                self.irq.eq(1),
                NextValue(self.done.status, 1),
                NextValue(self.error.status, 1),
                NextState("IDLE")
            )
        )
