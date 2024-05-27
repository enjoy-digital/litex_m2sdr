#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litepcie.common import *

from litex.soc.interconnect import stream

# UpSampling ---------------------------------------------------------------------------------------

class UpSampling(LiteXModule):
    def __init__(self, data_width=64):
        self.ratio  = Signal(4, reset=1)
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width))
        self.source = source = stream.Endpoint(dma_layout(data_width))

        # # #

        # Recopy Data.
        self.comb += source.data.eq(sink.data)

        # Throttle Valid/Ready
        count      = Signal(max=16)
        count_done = (count == (self.ratio - 1))
        self.sync += [
            If(self.ratio == 1,
                count.eq(0)
            ).Elif(source.valid & source.ready,
                count.eq(count + 1),
                If(count_done,
                    count.eq(0)
                )
            )
        ]
        self.comb += source.valid.eq(sink.valid)
        self.comb += source.first.eq(sink.first)
        self.comb += If(count_done, sink.ready.eq(source.ready))

# DownSampling -------------------------------------------------------------------------------------

class DownSampling(LiteXModule):
    def __init__(self, data_width=64):
        self.ratio  = Signal(4, reset=1)
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width))
        self.source = source = stream.Endpoint(dma_layout(data_width))

        # # #

        # Recopy Data
        self.comb += source.data.eq(sink.data)

        # Throttle Valid/Ready.
        count      = Signal(max=16)
        count_done = (count == (self.ratio - 1))
        self.sync += [
            If(self.ratio == 1,
                count.eq(0)
            ).Elif(sink.valid & sink.ready,
                count.eq(count + 1),
                If(count_done,
                    count.eq(0)
                )
            )
        ]
        self.comb += sink.ready.eq(1)
        self.comb += If(count_done,
            source.valid.eq(sink.valid),
            sink.ready.eq(source.ready)
        )
