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

from gateware.ad9361.phy import RFICPHY, phy_layout
from gateware.ad9361.spi import SPIMaster

# AD9361 PRBS Generator ----------------------------------------------------------------------------

class AD9361PRBSGenerator(LiteXModule):
    def __init__(self, seed=0x0a54):
        self.ce = Signal(reset=1)
        self.o  = Signal(16)

        # # #

        data = Signal(16, reset=seed)
        self.sync += If(self.ce, data.eq(Cat((
            data[1]  ^ data[2]  ^ data[4]  ^ data[5]  ^
            data[6]  ^ data[7]  ^ data[8]  ^ data[9]  ^
            data[10] ^ data[11] ^ data[12] ^ data[13] ^
            data[14] ^ data[15]),
            data[:-1]
            )
        ))
        self.comb += self.o.eq(data)

# AD9361 PRBS Checker ----------------------------------------------------------------------------

class AD9361PRBSChecker(LiteXModule):
    def __init__(self, seed=0x0a54):
        self.ce     = Signal(reset=1)
        self.i      = Signal(12)
        self.synced = Signal()

        # # #

        error = Signal()

        # # #

        # PRBS reference
        prbs = AD9361PRBSGenerator(seed=seed)
        prbs = ResetInserter()(prbs)
        self.submodules += prbs

        # PRBS re-synchronization.
        self.comb += prbs.ce.eq(self.ce)
        self.comb += prbs.reset.eq(error)

        # Error generation.
        self.comb += If(self.ce, error.eq(self.i != prbs.o[:12]))


        # Sync generation.
        self.sync_timer = WaitTimer(1024)
        self.comb += self.sync_timer.wait.eq(~error)
        self.comb += self.synced.eq(self.sync_timer.done)

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
            CSRField("rst_n",       size=1, offset=0, description="AD9361's active low reset, ``0``: Reset / ``1``: Enabled."),
            CSRField("enable",      size=1, offset=1, description="AD9361's enable, ``0``: Disabled / ``1``: Enabled."),
            CSRField("txnrx",       size=1, offset=4, description="AD9361's txnrx control."),
            CSRField("en_agc",      size=1, offset=5, description="AD9361's en_agc control."),
        ])
        self._ctrl = CSRStorage(4, description="AD9361's control pins.")
        self._stat = CSRStatus(8,  description="AD9361's status pins.")

        # # #

        self.cd_rfic = ClockDomain("rfic")
        self.cd_rfic.rst_buf = "bufg"

        # PHY --------------------------------------------------------------------------------------
        self.phy = RFICPHY(rfic_pads)

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

        # Data Flow --------------------------------------------------------------------------------
        self.comb += [
            self.phy.source.connect(rx_cdc.sink, keep={"valid", "ready"}),
            rx_cdc.sink.data[0*16:1*16].eq(self.phy.source.ia),
            rx_cdc.sink.data[1*16:2*16].eq(self.phy.source.qa),
            rx_cdc.sink.data[2*16:3*16].eq(self.phy.source.ib),
            rx_cdc.sink.data[3*16:4*16].eq(self.phy.source.qb),
        ]
        self.rx_pipeline = stream.Pipeline(
            rx_cdc,
            rx_buffer,
            self.source
        )

        self.tx_pipeline = stream.Pipeline(
            self.sink,
            tx_buffer,
            tx_cdc,
        )
        self.comb += [
            tx_cdc.source.connect(self.phy.sink, keep={"valid", "ready"}),
            self.phy.sink.ia.eq(tx_cdc.source.data[0*16:1*16]),
            self.phy.sink.qa.eq(tx_cdc.source.data[1*16:2*16]),
            self.phy.sink.ib.eq(tx_cdc.source.data[2*16:3*16]),
            self.phy.sink.qa.eq(tx_cdc.source.data[3*16:4*16]),
        ]

        # Config / Status --------------------------------------------------------------------------
        self.sync += [
            rfic_pads.rst_n.eq(self._config.fields.rst_n),
            rfic_pads.enable.eq(self._config.fields.enable),
            rfic_pads.txnrx.eq(self._config.fields.txnrx),
            rfic_pads.en_agc.eq(self._config.fields.en_agc),

            rfic_pads.ctrl.eq(self._ctrl.storage),
            self._stat.status.eq(rfic_pads.stat)
        ]

        # SPI --------------------------------------------------------------------------------------
        self.spi = SPIMaster(spi_pads, width=24, div=32)


    def add_prbs(self):
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
