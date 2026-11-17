#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream

# UDP Packet Dropper -------------------------------------------------------------------------------

class UDPPacketDropper(LiteXModule):
    """Drops whole UDP packets while the user behind it has no room for them.

    A LiteEth UDP port that stalls its source stalls the receive path of the whole core, with
    the other ports: over Ethernet the control link (Etherbone) goes through it. UDP has no flow
    control, so a full user loses data anyway: here it loses whole packets and never stalls.

    The decision is taken on the first word of a packet and held to its last word: ``room`` says
    that the user takes a whole packet without stalling.
    """
    def __init__(self, description):
        self.sink   = sink   = stream.Endpoint(description) # i
        self.source = source = stream.Endpoint(description) # o

        self.room   = Signal() # i
        self.drops  = Signal(32) # o: packets dropped.

        # # #

        first   = Signal(reset=1)
        drop    = Signal()
        drop_r  = Signal()
        self.comb += drop.eq(Mux(first, ~self.room, drop_r))
        self.sync += [
            If(sink.valid & sink.ready,
                first.eq(sink.last),
                If(first,
                    drop_r.eq(drop),
                    If(drop, self.drops.eq(self.drops + 1)),
                ),
            )
        ]
        self.comb += [
            sink.connect(source, omit={"valid", "ready"}),
            source.valid.eq(sink.valid & ~drop),
            sink.ready.eq(source.ready | drop),
        ]
