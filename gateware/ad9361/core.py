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
from gateware.sampling import UpSampling, DownSampling

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
        self._sampling = CSRStorage(fields=[
            CSRField("tx_ratio", size=4, offset=0, description="TX   Up-Sampling Ratio.", reset=1),
            CSRField("rx_ratio", size=4, offset=4, description="RX Down-Sampling Ratio.", reset=1),
        ])

        # # #

        # Clocking ---------------------------------------------------------------------------------
        self.cd_rfic = ClockDomain("rfic")

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

        # Up/Down-Sampling -------------------------------------------------------------------------
        self.tx_upsampling   = tx_upsampling   = UpSampling()
        self.rx_downsampling = rx_downsampling = DownSampling()
        self.comb += [
            self.tx_upsampling.ratio.eq(self._sampling.fields.tx_ratio),
            self.rx_downsampling.ratio.eq(self._sampling.fields.rx_ratio),
        ]

        # Buffers (For Timings) --------------------------------------------------------------------
        self.tx_buffer = tx_buffer = stream.Buffer(dma_layout(64))
        self.rx_buffer = rx_buffer = stream.Buffer(dma_layout(64))

        # Data Flow --------------------------------------------------------------------------------

        # TX.
        # ---
        def _16b_sign_extend(data):
            return Cat(data, Replicate(data[-1], 16 - len(data)))

        self.tx_pipeline = stream.Pipeline(
            self.sink,
            tx_buffer,
            tx_upsampling,
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
            rx_cdc.sink.data[0*16:1*16].eq(_16b_sign_extend(self.phy.source.ia)),
            rx_cdc.sink.data[1*16:2*16].eq(_16b_sign_extend(self.phy.source.qa)),
            rx_cdc.sink.data[2*16:3*16].eq(_16b_sign_extend(self.phy.source.ib)),
            rx_cdc.sink.data[3*16:4*16].eq(_16b_sign_extend(self.phy.source.qb)),
        ]
        self.rx_pipeline = stream.Pipeline(
            rx_cdc,
            rx_downsampling,
            rx_buffer,
            self.source
        )

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
        self.spi = SPIMaster(spi_pads, data_width=24, clk_divider=8)

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
