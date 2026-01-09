#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream

from litepcie.common import *

# Constants / Helpers ------------------------------------------------------------------------------

_16_BIT_MODE = 0
_8_BIT_MODE  = 1

def _sign_extend(data, nbits=16):
    return Cat(data, Replicate(data[-1], nbits - len(data)))

# AD9361 TX BitMode --------------------------------------------------------------------------------

class AD9361TXBitMode(LiteXModule):
    def __init__(self):
        self.sink   = sink   = stream.Endpoint(dma_layout(64))
        self.source = source = stream.Endpoint(dma_layout(64))
        self.mode   = mode   = Signal()

        # # #

        # 16-bit mode.
        # ------------
        self.comb += If(mode == _16_BIT_MODE, sink.connect(source))

        # 8-bit mode.
        # -----------
        self.conv = conv = stream.Converter(64, 32)
        self.comb += If(mode == _8_BIT_MODE,
            sink.connect(conv.sink),
            conv.source.connect(source, omit={"data"}),
            source.data[0*16+4:1*16].eq(_sign_extend(conv.source.data[0*8:1*8], 12)),
            source.data[1*16+4:2*16].eq(_sign_extend(conv.source.data[1*8:2*8], 12)),
            source.data[2*16+4:3*16].eq(_sign_extend(conv.source.data[2*8:3*8], 12)),
            source.data[3*16+4:4*16].eq(_sign_extend(conv.source.data[3*8:4*8], 12)),
        )

# AD9361 RX BitMode --------------------------------------------------------------------------------

class AD9361RXBitMode(LiteXModule):
    def __init__(self):
        self.sink   = sink   = stream.Endpoint(dma_layout(64))
        self.source = source = stream.Endpoint(dma_layout(64))
        self.mode   = mode   = Signal()

        # # #

        # 16-bit mode.
        # ------------
        self.comb += If(mode == _16_BIT_MODE, sink.connect(source))

        # 8-bit mode.
        # -----------
        self.conv = conv = stream.Converter(32, 64)
        self.comb += If(mode == _8_BIT_MODE,
            sink.connect(conv.sink, omit={"data"}),
            conv.sink.data[0*8:1*8].eq(sink.data[0*16+4:1*16]),
            conv.sink.data[1*8:2*8].eq(sink.data[1*16+4:2*16]),
            conv.sink.data[2*8:3*8].eq(sink.data[2*16+4:3*16]),
            conv.sink.data[3*8:4*8].eq(sink.data[3*16+4:4*16]),
            conv.source.connect(source),
        )
