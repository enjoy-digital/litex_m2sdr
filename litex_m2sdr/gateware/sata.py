# SPDX-License-Identifier: BSD-2-Clause
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>

from migen import *

from litex.gen import *
from litex.soc.interconnect import wishbone


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
