#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer
from litex.soc.interconnect import stream, wishbone


# PCIe Link Reset Workaround -----------------------------------------------------------------------

class PCIeLinkResetWorkaround(LiteXModule):
    """
    Periodically toggles PCIe reset until link-up is observed.

    Behavior:
    - If link is down on a timer tick, assert reset low.
    - On next timer tick, release reset high.
    - Once link is up, keep reset released.
    """
    def __init__(self, link_up, sys_clk_freq, interval_s=10e-3, interval_cycles=None):
        self.rst_n = Signal(reset=1)

        # # #

        # Synchronize link status into sys domain.
        link_up_sys = Signal()
        self.specials += MultiReg(link_up, link_up_sys)

        # Retry timer.
        if interval_cycles is None:
            interval_cycles = max(1, int(interval_s * sys_clk_freq))
        self.timer = timer = WaitTimer(interval_cycles)
        self.comb += timer.wait.eq(~timer.done)

        # Reset sequencing:
        # - If currently in reset, release it.
        # - Else, if link is still down, assert reset.
        self.sync += If(timer.done,
            If(~self.rst_n,
                self.rst_n.eq(1)
            ).Elif(~link_up_sys,
                self.rst_n.eq(0)
            )
        )


# PCIe Wishbone Slave With Burst Reads -------------------------------------------------------------

class LitePCIeWishboneBurstReadSlave(LiteXModule):
    def __init__(self, endpoint, address_width=32, data_width=32, addressing="word",
                 read_burst_dwords=128):
        assert data_width == 32

        self.bus = self.wishbone = bus = wishbone.Interface(
            address_width = address_width,
            data_width    = data_width,
            addressing    = addressing,
        )

        # # #

        # This bridge is used by the SATA host-memory DMA path, which performs
        # naturally aligned sequential reads from the kernel coherent buffer.
        read_burst_dwords = min(read_burst_dwords, 512//(data_width//8))
        ashift            = {"byte": 0, "word": log2_int(data_width//8)}[addressing]
        pcie_data_width   = endpoint.phy.data_width

        # Get Master port from Crossbar.
        port = endpoint.crossbar.get_master_port()

        # Completion data path.
        cmp_data = stream.Endpoint([("data", pcie_data_width)])
        conv     = ResetInserter()(stream.Converter(nbits_from=pcie_data_width, nbits_to=data_width))
        fifo     = ResetInserter()(stream.SyncFIFO([("data", data_width)], read_burst_dwords*2))
        self.conv = conv
        self.fifo = fifo
        self.comb += [
            cmp_data.connect(conv.sink),
            conv.source.connect(fifo.sink),
        ]

        store_rx = Signal()
        drop_rx  = Signal()
        self.comb += [
            cmp_data.valid.eq(port.sink.valid & store_rx),
            cmp_data.data.eq(port.sink.dat),
            port.sink.ready.eq(Mux(store_rx, cmp_data.ready, drop_rx)),
        ]

        read_base = Signal(address_width)
        next_addr = Signal(address_width)
        rx_active = Signal()
        clear_rx  = Signal()
        start_rx  = Signal()

        self.sync += [
            If(clear_rx,
                rx_active.eq(0)
            ).Elif(start_rx,
                rx_active.eq(1)
            ).Elif(port.sink.valid & port.sink.ready & port.sink.last & port.sink.end,
                rx_active.eq(0)
            )
        ]

        # Request common fields.
        req_adr = Signal(address_width)
        self.comb += [
            req_adr.eq(read_base << ashift),
            port.source.channel.eq(port.channel),
            port.source.req_id.eq(endpoint.phy.id),
            port.source.tag.eq(0),
            port.source.first.eq(1),
            port.source.last.eq(1),
            port.source.dat.eq(bus.dat_w),
        ]

        # Timeouts.
        self.req_timeout = req_timeout = WaitTimer(2**16)
        self.cmp_timeout = cmp_timeout = WaitTimer(2**18)

        # FSM.
        fsm = FSM(reset_state="IDLE")
        self.fsm = fsm

        fsm.act("IDLE",
            If(rx_active,
                store_rx.eq(1)
            ),
            If(bus.stb & bus.cyc,
                If(bus.we,
                    NextState("ISSUE-WRITE")
                ).Else(
                    If(fifo.source.valid & (bus.adr == next_addr),
                        bus.dat_r.eq(fifo.source.data),
                        bus.ack.eq(1),
                        fifo.source.ready.eq(1),
                        NextValue(next_addr, next_addr + 1),
                    ).Elif(rx_active,
                        NextState("DRAIN-READ")
                    ).Else(
                        fifo.reset.eq(1),
                        conv.reset.eq(1),
                        NextValue(read_base, bus.adr),
                        NextValue(next_addr, bus.adr),
                        NextState("ISSUE-READ")
                    )
                )
            )
        )
        fsm.act("DRAIN-READ",
            drop_rx.eq(1),
            fifo.reset.eq(1),
            conv.reset.eq(1),
            If(~rx_active,
                clear_rx.eq(1),
                NextState("IDLE")
            )
        )
        fsm.act("ISSUE-READ",
            req_timeout.wait.eq(1),
            port.source.valid.eq(1),
            port.source.we.eq(0),
            port.source.adr.eq(req_adr),
            port.source.len.eq(read_burst_dwords),
            If(port.source.ready,
                start_rx.eq(1),
                NextState("WAIT-FIRST")
            ).Elif(req_timeout.done,
                bus.ack.eq(1),
                bus.err.eq(1),
                NextState("IDLE")
            )
        )
        fsm.act("WAIT-FIRST",
            store_rx.eq(1),
            cmp_timeout.wait.eq(1),
            If(fifo.source.valid,
                bus.dat_r.eq(fifo.source.data),
                bus.ack.eq(1),
                fifo.source.ready.eq(1),
                NextValue(next_addr, read_base + 1),
                NextState("IDLE")
            ).Elif(cmp_timeout.done,
                clear_rx.eq(1),
                bus.ack.eq(1),
                bus.err.eq(1),
                NextState("IDLE")
            )
        )
        fsm.act("ISSUE-WRITE",
            req_timeout.wait.eq(1),
            port.source.valid.eq(1),
            port.source.we.eq(1),
            port.source.adr.eq(bus.adr << ashift),
            port.source.len.eq(1),
            If(port.source.ready | req_timeout.done,
                bus.ack.eq(1),
                bus.err.eq(req_timeout.done),
                NextState("IDLE")
            )
        )
