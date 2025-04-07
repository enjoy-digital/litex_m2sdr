#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect     import stream
from litex.soc.interconnect.csr import *

from litepcie.common import *

from litex_m2sdr.gateware.gpio import GPIORXPacker, GPIOTXUnpacker

from litex_m2sdr.gateware.ad9361.phy     import AD9361PHY
from litex_m2sdr.gateware.ad9361.spi     import AD9361SPIMaster
from litex_m2sdr.gateware.ad9361.bitmode import AD9361TXBitMode, AD9361RXBitMode
from litex_m2sdr.gateware.ad9361.bitmode import _sign_extend
from litex_m2sdr.gateware.ad9361.prbs    import AD9361PRBSGenerator, AD9361PRBSChecker
from litex_m2sdr.gateware.ad9361.agc     import AGCSaturationCount

# Architecture -------------------------------------------------------------------------------------
#
# The AD9361 PHY has the following simplified architecture:
#                                                                 ┌───────────────────┐
#                                                                 │                   │
#                                                                 │     SPI Core      ├──► SPI
#                                                                 │                   │
#                                                                 └───────────────────┘
#                               ┌────────────┐  ┌───┐
#                               │            │  │   │      ┌──────────────────────────┐
#                               │  RX PRBS   ◄──┤   │      │                          │
#                               │            │  │ D │      │          ┌───────────┐   │
#                               └────────────┘  │ E │      │          │  RX Data  │   │
#                                               │ M ◄──────┼──────────┤    2:1    ◄───┼── RX Data
#                ┌──────┐  ┌──────┐   ┌──────┐  │ U │      │          │    DDR    │   │
#     Source     │      │  │12-bit│   │      │  │ X │      │          └───────────┘   │
#    (To DMA) ◄──┤ BUF  ◄──┤ 8-bit◄───┤ CDC  ◄──┤   ◄─┐    │                    X6    │
#                │      │  │ mode │   │      │  │   │ │    │                          │  From AD9361
#                └──────┘  └──────┘   └──────┘  └───┘ │    │                          │
#                                                     │    │          ┌───────────┐   │
#                                                     │    │          │  RX Clk   │   │
#                                                     │    │      ┌───┤    BUF    ◄───┼── RX Clk
#                                                    T│    │      │   │           │   │
#                                                    X│    │      │   └───────────┘   │
#                                                    -│    │      │                   │
#                                                    R│    │      │                   │
#                                                    X│    │      │RFIC Clk           │
#                                                    -│    │      │                   │
#                                                    L│    │      │                   │
#                                                    o│    │      │                   │
#                                                    o│    │      │   ┌───────────┐   │
#                                                    p│    │      │   │  TX Clk   │   │
#                                                    b│    │      └───►    2:1    ├───┼─► TX Clk
#                                                    a│    │          │    DDR    │   │
#                                                    c│    │          └───────────┘   │
#                                                    k│    │                          │
#                              ┌────────────┐  ┌───┐  │    │                          │
#                              │            │  │   │  │    │                          │  To AD9361
#                              │  TX PRBS   ├──►   │  │    │                          │
#                              │            │  │   │  │    │          ┌───────────┐   │
#                              └────────────┘  │ M │  │    │          │  TX Data  │   │
#                                              │ U ├──┴────┼──────────►    2:1    ├───┼─► TX Data
#                ┌──────┐  ┌──────┐  ┌──────┐  │ X │       │          │    DDR    │   │
#    Sink        │      │  │12-bit│  │      │  │   │       │          └───────────┘   │
#   (From DMA) ──►  BUF ├──► 8-bit├──► CDC  ├──►   │       │                    X6    │
#                │      │  │ mode │  │      │  │   │       │                          │
#                └──────┘  └──────┘  └──────┘  └───┘       │            PHY           │
#                                                          └──────────────────────────┘
# - The rfic_clk is recovered from the AD9361 RX Clk through a Clk buffer.
# - The rfic_clk is used for both TX/RX.
# - 2:1 Serialization/Deserialiation is used on TX/RX.
# - RX sampling (on the FPGA) is adjusted through AD9361 registers.
# - TX sampling (on the AD931) is adjusted through AD9361 registers.
# - An optional TX-RX loopback is implemented.
# - Sink/Source stream operate in sys_clk domain @ 64-bit and are converted to/from rfic_clk.

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
                ("``0b0000``", "All control pins low."),
                ("``0b1111``", "All control pins high."),
            ], description="AD9361's control pins.")
        ])
        self._stat = CSRStatus(fields=[
            CSRField("stat", size=8, offset=0, values=[
                ("``0b00000000``", "All status pins low."),
                ("``0b11111111``", "All status pins high."),
            ], description="AD9361's status pins.")
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
            # AD9361 Control.
            rfic_pads.rst_n.eq(self._config.fields.rst_n),
            rfic_pads.enable.eq(self._config.fields.enable),
            rfic_pads.txnrx.eq(self._config.fields.txnrx),
            rfic_pads.en_agc.eq(self._config.fields.en_agc),

            # AD9361 Control/Status IOs.
            rfic_pads.ctrl.eq(self._ctrl.storage),
            self._stat.fields.stat.eq(rfic_pads.stat),
        ]

        # PHY --------------------------------------------------------------------------------------
        self.phy = AD9361PHY(rfic_pads)

        # TX/RX UnPacker/Packer (GPIOs) ------------------------------------------------------------

        self.gpio_tx_unpacker = gpio_tx_unpacker = GPIOTXUnpacker()
        self.gpio_rx_packer   = gpio_rx_packer   = GPIORXPacker()

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
        # Sink -> TX Buffer -> TX BitMode -> TX CDC -> GPIOTXUnpacker -> PHY.
        self.tx_pipeline = stream.Pipeline(
            self.sink,
            tx_buffer,
            tx_bitmode,
            tx_cdc,
            gpio_tx_unpacker,
        )
        self.comb += [
            gpio_tx_unpacker.source.connect(self.phy.sink, keep={"valid", "ready"}),
            self.phy.sink.ia.eq(gpio_tx_unpacker.source.data[0*16:1*16]),
            self.phy.sink.qa.eq(gpio_tx_unpacker.source.data[1*16:2*16]),
            self.phy.sink.ib.eq(gpio_tx_unpacker.source.data[2*16:3*16]),
            self.phy.sink.qb.eq(gpio_tx_unpacker.source.data[3*16:4*16]),
        ]

        # RX.
        # ---
        # PHY -> GPIORXUnpacker -> RX CDC -> RX BitMode -> RX Buffer -> Source.
        self.comb += [
            self.phy.source.connect(gpio_rx_packer.sink, keep={"valid", "ready"}),
            gpio_rx_packer.sink.data[0*16:1*16].eq(_sign_extend(self.phy.source.ia, 16)),
            gpio_rx_packer.sink.data[1*16:2*16].eq(_sign_extend(self.phy.source.qa, 16)),
            gpio_rx_packer.sink.data[2*16:3*16].eq(_sign_extend(self.phy.source.ib, 16)),
            gpio_rx_packer.sink.data[3*16:4*16].eq(_sign_extend(self.phy.source.qb, 16)),
        ]
        self.rx_pipeline = stream.Pipeline(
            gpio_rx_packer,
            rx_cdc,
            rx_bitmode,
            rx_buffer,
            self.source,
        )

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
        # --------
        prbs_generator = AD9361PRBSGenerator()
        prbs_generator = ResetInserter()(prbs_generator)
        prbs_generator = ClockDomainsRenamer("rfic")(prbs_generator)
        self.submodules += prbs_generator
        self.comb += [
            prbs_generator.reset.eq(~self.prbs_tx.fields.enable),
            prbs_generator.ce.eq(phy.sink.ready),
            If(self.prbs_tx.fields.enable,
                phy.sink.valid.eq(1),
                phy.sink.ia.eq(prbs_generator.o),
                phy.sink.ib.eq(prbs_generator.o),
            )
        ]

        # PRBS RX.
        # --------
        self.comb += self.prbs_rx.fields.synced.eq(1)
        for data in [phy.source.ia, phy.source.ib]:
            prbs_checker = AD9361PRBSChecker()
            prbs_checker = ClockDomainsRenamer("rfic")(prbs_checker)
            self.submodules += prbs_checker
            self.comb += [
                prbs_checker.i.eq(data),
                prbs_checker.ce.eq(phy.source.valid),
                If(~prbs_checker.synced,
                    self.prbs_rx.fields.synced.eq(0)
                ),
            ]

    def add_agc(self):
        rx_cdc = self.rx_cdc
        self.agc_count_rx1_low = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[0*16:1*16], rx_cdc.source.data[1*16:2*16]]
        )
        self.agc_count_rx1_high = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[0*16:1*16], rx_cdc.source.data[1*16:2*16]]
        )
        self.agc_count_rx2_low = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[2*16:3*16], rx_cdc.source.data[3*16:4*16]]
        )
        self.agc_count_rx2_high = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[2*16:3*16], rx_cdc.source.data[3*16:4*16]]
        )
