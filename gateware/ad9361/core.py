#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer

from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *

from litepcie.common import *

from gateware.ad9361.phy     import AD9361PHY
from gateware.ad9361.spi     import AD9361SPIMaster
from gateware.ad9361.bitmode import AD9361TXBitMode, AD9361RXBitMode
from gateware.ad9361.bitmode import _sign_extend

# AD9361 RFIC --------------------------------------------------------------------------------------

class AD9361RFIC(LiteXModule):
    def __init__(self, rfic_pads, spi_pads, sys_clk_freq):
        # Controls ---------------------------------------------------------------------------------
        self.enable_datapath = Signal(reset=1)

         # Stream Endpoints ------------------------------------------------------------------------
        self.sink   = stream.Endpoint(dma_layout(64))
        self.source = stream.Endpoint(dma_layout(64))

        # Config/Control/Status registers ----------------------------------------------------------
        self._config = CSRStorage(fields=[
            CSRField("rst_n",  size=1, offset=0, values=[
                ("``0b0``", "Reset the AD9361."),
                ("``0b1``", "Enable the AD9361."),
            ]),
            CSRField("enable", size=1, offset=1, values=[
                ("``0b0``", "AD9361 disabled."),
                ("``0b1``", "AD9361 enabled."),
            ]),
            CSRField("txnrx",  size=1, offset=4, values=[
                ("``0b0``", "Set to TX mode."),
                ("``0b1``", "Set to RX mode."),
            ]),
            CSRField("en_agc", size=1, offset=5, values=[
                ("``0b0``", "Disable AGC."),
                ("``0b1``", "Enable AGC."),
            ]),
        ])
        self._ctrl = CSRStorage(fields=[
            CSRField("ctrl", size=4, offset=0, values=[
                ("``0b0000``", "All control pins low"),
                ("``0b1111``", "All control pins high"),
            ], description="AD9361's control pins")
        ])
        self._stat = CSRStatus(fields=[
            CSRField("stat", size=8, offset=0, values=[
                ("``0b00000000``", "All status pins low"),
                ("``0b11111111``", "All status pins high"),
            ], description="AD9361's status pins")
        ])
        self._bitmode = CSRStorage(fields=[
            CSRField("mode", size=1, offset=0, values=[
                ("``0b0``", "12-bit mode."),
                ("``0b1``", " 8-bit mode."),
            ], description="Sample format.")
        ])

        # # #

        # Clocking ---------------------------------------------------------------------------------
        self.cd_rfic = ClockDomain("rfic")

        # SPI --------------------------------------------------------------------------------------
        self.spi = AD9361SPIMaster(spi_pads, data_width=24, clk_divider=8)

        # Config / Status --------------------------------------------------------------------------
        self.sync += [
            rfic_pads.rst_n.eq(self._config.fields.rst_n),
            rfic_pads.enable.eq(self._config.fields.enable),
            rfic_pads.txnrx.eq(self._config.fields.txnrx),
            rfic_pads.en_agc.eq(self._config.fields.en_agc),

            rfic_pads.ctrl.eq(self._ctrl.storage),
            self._stat.fields.stat.eq(rfic_pads.stat)
        ]

        # PHY --------------------------------------------------------------------------------------
        self.phy = AD9361PHY(rfic_pads)

        # Cross domain crossing --------------------------------------------------------------------
        self.tx_cdc = tx_cdc = stream.ClockDomainCrossing(
            layout  = dma_layout(64),
            cd_from = "sys",
            cd_to   = "rfic",
            with_common_rst = True
        )
        self.rx_cdc = rx_cdc = stream.ClockDomainCrossing(
            layout  = dma_layout(64),
            cd_from = "rfic",
            cd_to   = "sys",
            with_common_rst = True
        )

        # Buffers (For Timings) --------------------------------------------------------------------
        self.tx_buffer = tx_buffer = stream.Buffer(dma_layout(64))
        self.rx_buffer = rx_buffer = stream.Buffer(dma_layout(64))

        # BitMode ----------------------------------------------------------------------------------
        self.tx_bitmode = tx_bitmode = AD9361TXBitMode()
        self.rx_bitmode = rx_bitmode = AD9361RXBitMode()
        self.comb += tx_bitmode.mode.eq(self._bitmode.fields.mode)
        self.comb += rx_bitmode.mode.eq(self._bitmode.fields.mode)

        # Data Flow --------------------------------------------------------------------------------

        # TX.
        # ---
        self.tx_pipeline = stream.Pipeline(
            self.sink,
            tx_buffer,
            tx_bitmode,
            tx_cdc,
        )
        self.comb += [
            tx_cdc.source.connect(self.phy.sink, keep={"valid", "ready"}),
            self.phy.sink.ia.eq(tx_cdc.source.data[0*16:1*16]),
            self.phy.sink.qa.eq(tx_cdc.source.data[1*16:2*16]),
            self.phy.sink.ib.eq(tx_cdc.source.data[2*16:3*16]),
            self.phy.sink.qa.eq(tx_cdc.source.data[3*16:4*16]),
        ]

        # RX.
        # ---
        self.comb += [
            self.phy.source.connect(rx_cdc.sink, keep={"valid", "ready"}),
            rx_cdc.sink.data[0*16:1*16].eq(_sign_extend(self.phy.source.ia, 16)),
            rx_cdc.sink.data[1*16:2*16].eq(_sign_extend(self.phy.source.qa, 16)),
            rx_cdc.sink.data[2*16:3*16].eq(_sign_extend(self.phy.source.ib, 16)),
            rx_cdc.sink.data[3*16:4*16].eq(_sign_extend(self.phy.source.qb, 16)),
        ]
        self.rx_pipeline = stream.Pipeline(
            rx_cdc,
            rx_bitmode,
            rx_buffer,
            self.source
        )

    def add_prbs(self):
        from gateware.ad9361.prbs import AD9361PRBSGenerator, AD9361PRBSChecker
        self.prbs_tx = CSRStorage(fields=[
            CSRField("enable", size=1, offset= 0, values=[
                ("``0b0``", "Disable PRBS TX."),
                ("``0b1``", "Enable  PRBS TX."),
            ])])
        self.prbs_rx = CSRStatus(fields=[
            CSRField("synced", size=1, offset= 0, values=[
                ("``0b0``", "PRBS RX Out-of-Sync."),
                ("``0b1``", "PRBS_RX Synchronized."),
            ])])

        # # #

        phy = self.phy

        # PRBS TX.
        prbs_generator = AD9361PRBSGenerator()
        prbs_generator = ResetInserter()(prbs_generator)
        prbs_generator = ClockDomainsRenamer("rfic")(prbs_generator)
        self.comb += prbs_generator.reset.eq(~self.prbs_tx.fields.enable)
        self.submodules += prbs_generator
        self.comb += prbs_generator.ce.eq(phy.sink.ready)
        self.comb += If(self.prbs_tx.fields.enable,
            phy.sink.valid.eq(1),
            phy.sink.ia.eq(prbs_generator.o),
            phy.sink.ib.eq(prbs_generator.o),
        )

        # PRBS RX.
        self.comb += self.prbs_rx.fields.synced.eq(1)
        for data in [phy.source.ia, phy.source.ib]:
            prbs_checker = AD9361PRBSChecker()
            prbs_checker = ClockDomainsRenamer("rfic")(prbs_checker)
            self.submodules += prbs_checker
            self.comb += prbs_checker.i.eq(data)
            self.comb += prbs_checker.ce.eq(phy.source.valid)
            self.comb += If(~prbs_checker.synced, self.prbs_rx.fields.synced.eq(0))
