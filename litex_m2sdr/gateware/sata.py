# SPDX-License-Identifier: BSD-2-Clause
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>

from migen import *
from math import log2

from litex.gen import *
from litex.gen.common import reverse_bytes
from litex.soc.cores.dma import WishboneDMAReader, WishboneDMAWriter
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *
from litex.soc.interconnect import wishbone

from litesata.common import logical_sector_size


SATA_HOST_BUFFER_BASE = 0x00020000
SATA_HOST_BUFFER_SIZE = 64 * 1024


class SATAHostBuffer(LiteXModule):
    def __init__(self, size=SATA_HOST_BUFFER_SIZE):
        self.host_bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self.dma_bus  = wishbone.Interface(data_width=32, address_width=32, addressing="word")

        # # #

        depth = size // 4
        mem = Memory(32, depth, name="sata_host_buffer")
        self.specials += mem

        self._add_port(mem, self.host_bus)
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


class M2SDRLiteSATASector2MemDMA(LiteXModule):
    """LiteSATA Sector2Mem DMA with contiguous multi-sector host-buffer reads."""
    def __init__(self, port, bus, endianness="little"):
        self.port     = port
        self.bus      = bus
        self.sector   = CSRStorage(48)
        self.nsectors = CSRStorage(16)
        self.base     = CSRStorage(64)
        self.start    = CSR()
        self.done     = CSRStatus()
        self.error    = CSRStatus()
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

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(count,             0),
                NextValue(total_words,       self.nsectors.storage * (logical_sector_size//dma_bytes)),
                NextValue(self.error.status, 0),
                NextState("SEND-CMD")
            ).Else(
                self.done.status.eq(1)
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
            dma.sink.address.eq(self.base.storage[int(log2(dma_bytes)):] + count),
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
                NextValue(self.error.status, 1),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-RESPONSE",
            source_ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.read & port.source.end,
                If(port.source.failed,
                    NextValue(self.error.status, 1),
                ),
                self.irq.eq(1),
                NextState("IDLE")
            ),
            If(port.source.valid & port.source.ready & port.source.failed,
                self.irq.eq(1),
                NextValue(self.error.status, 1),
                NextState("IDLE"),
            )
        )


class M2SDRLiteSATAMem2SectorDMA(LiteXModule):
    """LiteSATA Mem2Sector DMA with valid-gated SATA sink handshakes."""
    def __init__(self, bus, port, endianness="little"):
        self.bus      = bus
        self.port     = port
        self.sector   = CSRStorage(48)
        self.nsectors = CSRStorage(16)
        self.base     = CSRStorage(64)
        self.start    = CSR()
        self.done     = CSRStatus()
        self.error    = CSRStatus()
        self.irq      = Signal()

        # # #

        dma_bytes  = bus.data_width//8
        port_bytes = port.dw//8
        count      = Signal(max=logical_sector_size//min(dma_bytes, port_bytes))
        crt_sec    = Signal(48)
        crt_base   = Signal(64)

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

        # Control FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.start.re,
                NextValue(count,             0),
                NextValue(crt_sec,           self.sector.storage),
                NextValue(crt_base,          self.base.storage),
                NextValue(self.error.status, 0),
                NextState("READ-DATA-DMA")
            ).Else(
                self.done.status.eq(1)
            ),
            conv.source.ready.eq(1)
        )
        fsm.act("READ-DATA-DMA",
            # Read sector data over DMA.
            dma.sink.valid.eq(1),
            dma.sink.address.eq(crt_base[int(log2(dma_bytes)):] + count),
            If(dma.sink.valid & dma.sink.ready,
                NextValue(count, count + 1),
                If(count == (logical_sector_size//dma_bytes - 1),
                    NextValue(count, 0),
                    NextState("SEND-CMD-AND-DATA")
                )
            )
        )
        fsm.act("SEND-CMD-AND-DATA",
            # Send write command/data for 1 sector. Gate valid with the
            # converter output so the first SATA beat cannot use stale data.
            port.sink.valid.eq(conv.source.valid),
            port.sink.last.eq(count == (logical_sector_size//port_bytes - 1)),
            port.sink.write.eq(1),
            port.sink.sector.eq(crt_sec),
            port.sink.count.eq(1),
            port.sink.data.eq(reverse_bytes(conv.source.data)),
            If(port.sink.valid & port.sink.ready,
                conv.source.ready.eq(1),
                NextValue(count, count + 1),
                If(port.sink.last,
                    NextState("WAIT-ACK")
                )
            ),

            # Monitor errors.
            port.source.ready.eq(1),
            If(port.source.valid & port.source.ready & port.source.failed,
                self.irq.eq(1),
                NextValue(self.error.status, 1),
                NextState("IDLE"),
            )
        )
        fsm.act("WAIT-ACK",
            port.source.ready.eq(1),
            If(port.source.valid,
                If(port.source.failed,
                    NextValue(self.error.status, 1),
                    self.irq.eq(1),
                    NextState("IDLE")
                ).Elif(crt_sec == (self.sector.storage + self.nsectors.storage - 1),
                    self.irq.eq(1),
                    NextState("IDLE")
                ).Else(
                    NextValue(count,    0),
                    NextValue(crt_sec,  crt_sec + 1),
                    NextValue(crt_base, crt_base + 512),
                    NextState("READ-DATA-DMA")
                )
            )
        )


def install_litesata_dma_patches():
    import litesata.frontend.dma

    litesata.frontend.dma.LiteSATASector2MemDMA = M2SDRLiteSATASector2MemDMA
    litesata.frontend.dma.LiteSATAMem2SectorDMA = M2SDRLiteSATAMem2SectorDMA
