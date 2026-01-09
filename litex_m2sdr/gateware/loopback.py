#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *

from litepcie.common import dma_layout

# TX/RX Loopback ----------------------------------------------------------------------------------

class TXRXLoopback(LiteXModule):
    def __init__(self, data_width=64, with_csr=True):
        assert data_width == 64
        self.tx_sink   = tx_sink   = stream.Endpoint(dma_layout(data_width)) # i
        self.tx_source = tx_source = stream.Endpoint(dma_layout(data_width)) # o
        self.rx_sink   = rx_sink   = stream.Endpoint(dma_layout(data_width)) # i
        self.rx_source = rx_source = stream.Endpoint(dma_layout(data_width)) # o

        self.enable = Signal() # i (CSR).

        if with_csr:
            self.add_csr()

        # # #

        # Default: straight-through.
        # --------------------------
        self.comb += [
            If(self.enable,
                # Loopback: TX -> RX.
                # -------------------
                tx_sink.connect(rx_source),

                # Disable TX output to RFIC.
                tx_source.valid.eq(0),

                # Drain/ignore live RFIC RX.
                rx_sink.ready.eq(1),
            ).Else(
                # Normal: TX -> TX, RX -> RX.
                # ---------------------------
                tx_sink.connect(tx_source),
                rx_sink.connect(rx_source),
            )
        ]

    def add_csr(self, default_enable=0):
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "Normal: TX->RFIC and RFIC->RX."),
                ("``0b1``", "Loopback: TX stream routed into RX stream."),
            ], reset=default_enable),
        ])
        self.comb += self.enable.eq(self._control.fields.enable)
