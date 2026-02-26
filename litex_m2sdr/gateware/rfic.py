#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2025-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect import packet, stream


class RFICDataFramer(LiteXModule):
    def __init__(self, data_width=32, data_words=32):
        self.sink = sink = stream.Endpoint([("data", data_width)])
        self.source = source = stream.Endpoint([("data", data_width)])

        count = Signal(16)
        last = Signal()

        self.comb += [
            sink.connect(source),
            last.eq(count == (data_words - 1)),
            source.last.eq(last),
        ]

        self.sync += If(sink.valid & sink.ready,
            If(last,
                count.eq(0)
            ).Else(
                count.eq(count + 1)
            )
        )


class RFICDataPacketizer(LiteXModule):
    def __init__(self, data_width=32, data_words=256):
        self.sink = sink = stream.Endpoint([("data", data_width)])
        self.source = source = stream.Endpoint([("data", data_width), ("data_words", 16)])

        self.data_framer = RFICDataFramer(data_width=data_width, data_words=data_words)
        self.data_fifo = packet.PacketFIFO(
            layout=[("data", data_width)],
            payload_depth=data_words * 2,
            param_depth=None,
            buffered=True,
        )

        self.submodules += stream.Pipeline(
            sink,
            self.data_framer,
            self.data_fifo,
            source,
        )
        self.comb += source.data_words.eq(data_words)

