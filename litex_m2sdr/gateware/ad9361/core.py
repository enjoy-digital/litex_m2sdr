#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litex.soc.interconnect     import stream
from litex.soc.interconnect.csr import *

from litepcie.common import *

from litex_m2sdr.gateware.gpio import GPIORXPacker, GPIOTXUnpacker

from litex_m2sdr.gateware.ad9361.phy     import AD9361PHY
from litex_m2sdr.gateware.ad9361.cdc     import ClockDomainCrossingRegistered
from litex_m2sdr.gateware.ad9361.spi     import AD9361SPIMaster
from litex_m2sdr.gateware.ad9361.bitmode import AD9361TXBitMode, AD9361RXBitMode
from litex_m2sdr.gateware.ad9361.bitmode import _sign_extend
from litex_m2sdr.gateware.ad9361.prbs    import AD9361PRBSGenerator, AD9361PRBSChecker
from litex_m2sdr.gateware.ad9361.prbs    import AD9361PRBS1R1TGenerator, AD9361PRBS1R1TChecker
from litex_m2sdr.gateware.ad9361.agc     import (
    AGC_DEFAULT_HIGH_THRESHOLD,
    AGC_DEFAULT_LOW_THRESHOLD,
    AGCSaturationCount,
)

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

class AD9361RFICStreamBypass(LiteXModule):
    def __init__(self, data_width=64):
        self.sink   = stream.Endpoint(dma_layout(data_width))
        self.source = stream.Endpoint(dma_layout(data_width))

        # # #

        self.comb += self.sink.connect(self.source)


class AD9361RFIC(LiteXModule):
    def __init__(self, rfic_pads, spi_pads, sys_clk_freq,
        with_tx_fifo      = False, tx_fifo_depth = 8192,
        with_rx_fifo      = False, rx_fifo_depth = 8192,
        with_phy_loopback = True,
        with_rx_deskew    = False,
        with_tx_phase     = False,
        with_tx_oserdes   = False,
        wide              = False):
        # wide=True runs the DMA-facing datapath (sink/source, buffers, bitmode, CDC) at 128-bit
        # = two (ia,qa,ib,qb) groups per word, halving the sys-domain word rate so 2T2R @ 122.88
        # MSPS fits the 64-bit @ 125MHz fabric. The PHY serializer stays 64-bit (one group); a
        # 128->64 scatter (TX) and 64->128 gather (RX) sit at the PHY boundary in the rfic domain
        # (which has 4x headroom at 491.52MHz). Used by the Oversampling build only.
        self.wide = wide
        dw = 128 if wide else 64
        # Stream Endpoints -------------------------------------------------------------------------
        self.sink   = stream.Endpoint(dma_layout(dw))
        self.source = stream.Endpoint(dma_layout(dw))

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
            CSRField("mode", size=2, offset=0, values=[
                ("``0b00``", "12-bit mode in SC16/Q11 transport containers."),
                ("``0b01``", " 8-bit mode in SC8/Q7 transport containers."),
                ("``0b10``", "BFP8 block-floating transport mode."),
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
        self.phy = AD9361PHY(rfic_pads, with_loopback=with_phy_loopback,
            with_rx_idelay=with_rx_deskew, with_tx_phase=with_tx_phase, with_tx_oserdes=with_tx_oserdes)
        if with_rx_deskew:
            self.add_rx_deskew()

        # TX/RX UnPacker/Packer (GPIOs) ------------------------------------------------------------

        self.gpio_tx_unpacker = gpio_tx_unpacker = GPIOTXUnpacker()
        self.gpio_rx_packer   = gpio_rx_packer   = GPIORXPacker()

        # Cross domain crossing --------------------------------------------------------------------
        # Registered-flags CDC FIFOs: keep the pointer synchronizers out of the memory port
        # address/enable cones so the rfic side closes timing at 491.52MHz (see cdc.py).
        self.tx_cdc = tx_cdc = ClockDomainCrossingRegistered(
            layout  = dma_layout(dw),
            cd_from = "sys",
            cd_to   = "rfic",
            # Read in the rfic domain at 491.52MHz (Oversampling): registered_read selects the
            # 128-bit mux with the REGISTERED read pointer (reg -> mux -> reg) so the read closes
            # single-cycle at this rate (see cdc.py); the one-cycle bubble it introduces is harmless
            # since the scatter pulls only ~1-in-8. register_storage uses an FF array (LUTRAM's write
            # clock would violate the RAMD32 min low-pulse-width at 491.52MHz).
            register_storage = True,
            registered_read  = True,
        )
        self.rx_cdc = rx_cdc = ClockDomainCrossingRegistered(
            layout  = dma_layout(dw),
            cd_from = "rfic",
            cd_to   = "sys",
            # rfic_clk-clocked LUTRAM writes violate the RAMD32 min low-pulse-width at 491.52MHz.
            register_storage = True,
        )

        # Buffers (For Timings) --------------------------------------------------------------------
        self.tx_buffer = tx_buffer = stream.Buffer(dma_layout(dw))
        self.rx_buffer = rx_buffer = stream.Buffer(dma_layout(dw))
        self.tx_rfic_fifo_started = tx_rfic_fifo_started = Signal()
        if with_tx_fifo:
            self.tx_rfic_fifo = tx_rfic_fifo = ClockDomainsRenamer("rfic")(
                stream.SyncFIFO(dma_layout(dw), depth=tx_fifo_depth, buffered=True)
            )
            tx_fifo_start_level = max(1, tx_fifo_depth//2)
            tx_rfic_fifo_primed = Signal()
            self.comb += tx_rfic_fifo_primed.eq(tx_rfic_fifo.level >= tx_fifo_start_level)
            self.sync.rfic += [
                If(tx_rfic_fifo_primed,
                    tx_rfic_fifo_started.eq(1)
                ).Elif(tx_rfic_fifo.level == 0,
                    tx_rfic_fifo_started.eq(0)
                )
            ]
        else:
            self.tx_rfic_fifo = tx_rfic_fifo = AD9361RFICStreamBypass(dw)
            self.comb += tx_rfic_fifo_started.eq(1)

        if with_rx_fifo:
            self.rx_rfic_fifo = rx_rfic_fifo = ClockDomainsRenamer("rfic")(
                stream.SyncFIFO(dma_layout(dw), depth=rx_fifo_depth, buffered=True)
            )
        else:
            self.rx_rfic_fifo = rx_rfic_fifo = AD9361RFICStreamBypass(dw)

        # BitMode ----------------------------------------------------------------------------------
        self.tx_bitmode = tx_bitmode = AD9361TXBitMode(wide=wide)
        self.rx_bitmode = rx_bitmode = AD9361RXBitMode(wide=wide)
        self.comb += tx_bitmode.mode.eq(self._bitmode.fields.mode)
        self.comb += rx_bitmode.mode.eq(self._bitmode.fields.mode)

        # Data Flow --------------------------------------------------------------------------------

        # TX.
        # ---
        # Sink -> TX Buffer -> TX BitMode -> TX CDC -> optional TX RFIC FIFO -> [wide: 128->64
        # scatter] -> GPIOTXUnpacker -> PHY. The PHY serializer is always 64-bit (one group); in
        # wide mode the scatter splits each 128-bit (two-group) word into two 64-bit words in the
        # rfic domain (which has 4x headroom at 491.52MHz).
        tx_stages = [self.sink, tx_buffer, tx_bitmode, tx_cdc, tx_rfic_fifo]
        if wide:
            self.tx_scatter = tx_scatter = ClockDomainsRenamer("rfic")(stream.Converter(128, 64))
            tx_stages.append(tx_scatter)
        tx_stages.append(gpio_tx_unpacker)
        self.tx_pipeline = stream.Pipeline(*tx_stages)
        self.comb += [
            self.phy.sink.valid.eq(gpio_tx_unpacker.source.valid & tx_rfic_fifo_started),
            gpio_tx_unpacker.source.ready.eq(self.phy.sink.ready & tx_rfic_fifo_started),
            self.phy.sink.ia.eq(gpio_tx_unpacker.source.data[0*16:1*16]),
            self.phy.sink.qa.eq(gpio_tx_unpacker.source.data[1*16:2*16]),
            self.phy.sink.ib.eq(gpio_tx_unpacker.source.data[2*16:3*16]),
            self.phy.sink.qb.eq(gpio_tx_unpacker.source.data[3*16:4*16]),
        ]

        # RX.
        # ---
        # PHY -> GPIORXPacker -> [wide: 64->128 gather] -> optional RX RFIC FIFO -> RX CDC ->
        # RX BitMode -> RX Buffer -> Source. The gather pairs two 64-bit (one-group) words into a
        # 128-bit (two-group) word in the rfic domain.
        self.comb += [
            self.phy.source.connect(gpio_rx_packer.sink, keep={"valid", "ready"}),
            gpio_rx_packer.sink.data[0*16:1*16].eq(_sign_extend(self.phy.source.ia, 16)),
            gpio_rx_packer.sink.data[1*16:2*16].eq(_sign_extend(self.phy.source.qa, 16)),
            gpio_rx_packer.sink.data[2*16:3*16].eq(_sign_extend(self.phy.source.ib, 16)),
            gpio_rx_packer.sink.data[3*16:4*16].eq(_sign_extend(self.phy.source.qb, 16)),
        ]
        rx_stages = [gpio_rx_packer]
        if wide:
            # Register the rfic-domain RX stream BEFORE the gather. rx_valid (phy.source.valid)
            # is a PHY-domain FF that the placer clusters near the LVDS IOBs; it fans out to the
            # 64->128 gather's 128-bit output-register clears (source_payload_data[*].R), and that
            # long IOB->fabric route is the rfic_clk WNS limiter at 491.52MHz (DDR-frame build:
            # -0.343 on exactly this path). A local Buffer (same "for timings" pattern as
            # tx_buffer/rx_buffer) lets the gather be driven by a fabric FF placed next to it.
            self.rx_gather_ibuf = rx_gather_ibuf = ClockDomainsRenamer("rfic")(stream.Buffer(dma_layout(64)))
            rx_stages.append(rx_gather_ibuf)
            self.rx_gather = rx_gather = ClockDomainsRenamer("rfic")(stream.Converter(64, 128))
            rx_stages.append(rx_gather)
        rx_stages += [rx_rfic_fifo, rx_cdc, rx_bitmode, rx_buffer, self.source]
        self.rx_pipeline = stream.Pipeline(*rx_stages)

    def add_rx_deskew(self):
        """Per-lane RX deskew: IDELAYE2 tap loading + per-lane error counters.

        The error counters compare the channel 1 and channel 2 sample slots: with the AD9361 BIST
        PRBS enabled in 2R2T mode the chip sends the same word in both slots, so any ch1/ch2
        difference on a lane's two word bits is a capture error on that lane. Software sweeps the
        IDELAYE2 taps and reads the counters to center each lane in its eye.
        """
        self.rx_deskew_idelay = CSRStorage(fields=[
            CSRField("value", size=5, offset=0, description="IDELAYE2 tap value to load."),
            CSRField("lane",  size=3, offset=8, description="RX data lane to load (0-5)."),
        ], description="Load an IDELAYE2 tap value on an RX data lane (applies on write).")
        self.rx_deskew_ctrl = CSRStorage(fields=[
            CSRField("latch", size=1, offset=0, pulse=True,
                description="Latch the per-lane error counters and clear them."),
        ])
        self.rx_deskew_err = []
        for i in range(6):
            csr = CSRStatus(24, name=f"rx_deskew_err{i}",
                description=f"Latched lane {i} error count.")
            setattr(self, f"rx_deskew_err{i}", csr)
            self.rx_deskew_err.append(csr)

        # # #

        phy = self.phy

        # IDELAYE2 loading: the storage fields update at the end of the write cycle, so strobe LD
        # one cycle later when value/lane already hold the written values.
        idelay_re_d = Signal()
        self.sync += idelay_re_d.eq(self.rx_deskew_idelay.re)
        self.comb += phy.rx_idelay_value.eq(self.rx_deskew_idelay.fields.value)
        for i in range(6):
            self.comb += phy.rx_idelay_ld[i].eq(idelay_re_d & (self.rx_deskew_idelay.fields.lane == i))

        # Per-lane ch1-vs-ch2 error counters (rfic domain, pipelined for 491.52MHz DATA_CLK).
        valid_r = Signal()
        ia_r    = Signal(12)
        qa_r    = Signal(12)
        ib_r    = Signal(12)
        qb_r    = Signal(12)
        self.sync.rfic += [
            valid_r.eq(phy.source.valid),
            If(phy.source.valid,
                ia_r.eq(phy.source.ia),
                qa_r.eq(phy.source.qa),
                ib_r.eq(phy.source.ib),
                qb_r.eq(phy.source.qb),
            )
        ]
        lane_err = Signal(6)
        for i in range(6):
            self.sync.rfic += lane_err[i].eq(valid_r & (
                (ia_r[i] ^ ib_r[i]) | (ia_r[i+6] ^ ib_r[i+6]) |
                (qa_r[i] ^ qb_r[i]) | (qa_r[i+6] ^ qb_r[i+6])
            ))

        latch_rfic = PulseSynchronizer("sys", "rfic")
        self.submodules += latch_rfic
        self.comb += latch_rfic.i.eq(self.rx_deskew_ctrl.fields.latch)
        for i in range(6):
            count = Signal(24)
            hold  = Signal(24)
            self.sync.rfic += [
                If(lane_err[i],
                    count.eq(count + 1)
                ),
                If(latch_rfic.o,
                    hold.eq(count),
                    count.eq(0),
                )
            ]
            # hold is stable between latches; sample it through a MultiReg.
            self.specials += MultiReg(hold, self.rx_deskew_err[i].status)

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
        mode_rfic = Signal()
        prbs_tx_enable = Signal()
        self.specials += [
            MultiReg(phy.control.fields.mode, mode_rfic, odomain="rfic"),
            MultiReg(self.prbs_tx.fields.enable, prbs_tx_enable, odomain="rfic"),
        ]

        # PRBS TX.
        # --------
        prbs_generator_2r2t = AD9361PRBSGenerator()
        prbs_generator_2r2t = ResetInserter()(prbs_generator_2r2t)
        prbs_generator_2r2t = ClockDomainsRenamer("rfic")(prbs_generator_2r2t)
        prbs_generator_1r1t = AD9361PRBS1R1TGenerator()
        prbs_generator_1r1t = ResetInserter()(prbs_generator_1r1t)
        prbs_generator_1r1t = ClockDomainsRenamer("rfic")(prbs_generator_1r1t)
        self.submodules += prbs_generator_2r2t
        self.submodules += prbs_generator_1r1t
        self.comb += [
            prbs_generator_2r2t.reset.eq(~prbs_tx_enable | mode_rfic),
            prbs_generator_1r1t.reset.eq(~prbs_tx_enable | ~mode_rfic),
            prbs_generator_2r2t.ce.eq(phy.sink.ready),
            prbs_generator_1r1t.ce.eq(phy.sink.ready),
            If(prbs_tx_enable,
                phy.sink.valid.eq(1),
                If(mode_rfic,
                    phy.sink.ia.eq(prbs_generator_1r1t.o),
                    phy.sink.qa.eq(prbs_generator_1r1t.o),
                    phy.sink.ib.eq(prbs_generator_1r1t.o_next),
                    phy.sink.qb.eq(prbs_generator_1r1t.o_next),
                ).Else(
                    phy.sink.ia.eq(prbs_generator_2r2t.o),
                    phy.sink.qa.eq(prbs_generator_2r2t.o),
                    phy.sink.ib.eq(prbs_generator_2r2t.o),
                    phy.sink.qb.eq(prbs_generator_2r2t.o),
                )
            )
        ]

        # PRBS RX.
        # --------
        # 2R2T mode: each lane (channel) carries the full PRBS sequence; check
        # ia/ib independently. 1R1T mode: the a/b slots carry two consecutive
        # samples of ONE stream (each lane sees every other PRBS value), so a
        # dedicated interleaved checker is required. Select by PHY mode.

        synced_2r2t = Signal()
        self.comb += synced_2r2t.eq(1)  # default; cleared below when a lane checker is unsynced
        for data in [phy.source.ia, phy.source.ib]:
            prbs_checker = AD9361PRBSChecker()
            prbs_checker = ClockDomainsRenamer("rfic")(prbs_checker)
            self.submodules += prbs_checker
            self.comb += [
                prbs_checker.i.eq(data),
                prbs_checker.ce.eq(phy.source.valid),
                If(~prbs_checker.synced,
                    synced_2r2t.eq(0)
                ),
            ]

        prbs_checker_1r1t = AD9361PRBS1R1TChecker()
        prbs_checker_1r1t = ClockDomainsRenamer("rfic")(prbs_checker_1r1t)
        self.submodules += prbs_checker_1r1t
        self.comb += [
            prbs_checker_1r1t.ia.eq(phy.source.ia),
            prbs_checker_1r1t.ib.eq(phy.source.ib),
            prbs_checker_1r1t.ce.eq(phy.source.valid),
        ]

        synced = Signal()
        self.comb += synced.eq(Mux(mode_rfic, prbs_checker_1r1t.synced, synced_2r2t))
        self.specials += MultiReg(synced, self.prbs_rx.fields.synced)

    def add_agc(self):
        rx_cdc = self.rx_cdc
        self.agc_count_rx1_low = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[0*16:1*16], rx_cdc.source.data[1*16:2*16]],
            threshold_reset = AGC_DEFAULT_LOW_THRESHOLD,
        )
        self.agc_count_rx1_high = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[0*16:1*16], rx_cdc.source.data[1*16:2*16]],
            threshold_reset = AGC_DEFAULT_HIGH_THRESHOLD,
        )
        self.agc_count_rx2_low = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[2*16:3*16], rx_cdc.source.data[3*16:4*16]],
            threshold_reset = AGC_DEFAULT_LOW_THRESHOLD,
        )
        self.agc_count_rx2_high = AGCSaturationCount(
            ce  = rx_cdc.source.valid & rx_cdc.source.ready,
            iqs = [rx_cdc.source.data[2*16:3*16], rx_cdc.source.data[3*16:4*16]],
            threshold_reset = AGC_DEFAULT_HIGH_THRESHOLD,
        )
