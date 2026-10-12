#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc       import MultiReg, PulseSynchronizer
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litex.soc.cores.clock import S7MMCM

from litex_m2sdr.gateware.cdc import AsyncFIFORegistered

from litex_m2sdr.gateware.ad9361.phy import phy_layout
from litex_m2sdr.gateware.ad9361.phy import AD9361PHY1R1T_MODE, AD9361PHY2R2T_MODE

"""
AD9361 RFIC Oversampling PHY (Dual Port Full Duplex LVDS Mode, 2T2R @ 122.88 MSPS).

The high-rate companion of AD9361PHY (phy.py), used by the --with-rfic-oversampling build. At
2T2R @ 122.88 MSPS the interface DATA_CLK doubles to 491.52 MHz and each LVDS lane carries
983 Mbps (DDR); at that rate the simple PHY's assumptions no longer hold, so this PHY differs
in three ways (everything else - stream interface, framing, interleave - is identical):

- TX via OSERDESE2: the six TX data lanes + TX_FRAME are serialized by OSERDESE2 (8:1 DDR,
  CLK 491.52 MHz / CLKDIV 122.88 MHz from a dedicated MMCM). A fabric-clocked ODDR's output eye
  is too poor at 983 Mbps for the AD9361's 8-way 2R2T de-interleave (a clean tone splatters to
  fs/2); the OSERDESE2 eye is clean. The serialized bit order is identical to the simple PHY's
  ODDR serializer. The MMCM only locks at the 491.52 MHz DATA_CLK, so this PHY transmits only
  at 2T2R @ 122.88 MSPS; the simple PHY covers all lower rates.
- Per-lane RX deskew: an IDELAYE2 (VAR_LOAD) on each RX data lane, with per-lane pair-mismatch
  counters. The board's lane-to-lane skew exceeds the 1.017 ns UI eye at 983 Mbps and the
  AD9361's delay registers shift all lanes together, so software sweeps the taps per lane at
  bring-up (requires the design's IDELAYCTRL with its 200 MHz reference).
- RX frame-slot rotation (control.rx_frame_offset): RX_FRAME carries no per-lane delay, so on a
  board whose frame-lane skew differs from the data lanes the 2R2T slot decode can land a whole
  DATA_CLK cycle off - a rotation of the assembled words that the per-lane pair metric cannot
  see. Software sweeps the rotation against the sequence-level PRBS verify at bring-up.

No internal TX -> RX loopback: its skid + high-fanout control net do not close timing at
491.52 MHz rfic_clk and the 2T2R @ 122.88 datapath needs that margin. The `loopback` control
field is kept for register-layout compatibility and has no effect.
"""

# AD9361 Oversampling PHY --------------------------------------------------------------------------

class AD9361OversamplingPHY(LiteXModule):
    """
    AD9361 RFIC Oversampling PHY Module (2T2R @ 122.88 MSPS).

    Same stream interface and interleave as AD9361PHY; TX through OSERDESE2 serializers, RX
    through per-lane IDELAYE2 deskew. See the module docstring for the differences and limits.

    Parameters:
        pads: External FPGA pins connected to the AD9361.
    """
    def __init__(self, pads):
        self.sink    = sink   = stream.Endpoint(phy_layout()) # TX input  stream.
        self.source  = source = stream.Endpoint(phy_layout()) # RX output stream.
        self.control = CSRStorage(fields=[
            CSRField("mode", size=1, offset=0, values=[
                ("``0b0``", "2R2T mode (8-way interleaving)"),
                ("``0b1``", "1R1T mode (4-way interleaving)"),
            ], description="Selects between 1R1T and 2R2T modes"),
            CSRField("loopback", size=1, offset=1, description=
                "Reserved on this PHY (no internal loopback at 491.52 MHz); kept for "
                "register-layout compatibility with the standard PHY."),
            CSRField("rx_frame_offset", size=2, offset=4, reset=0, description=
                "Rotate the RX slot decode by N DATA_CLK cycles relative to the RX_FRAME edge "
                "(2R2T). RX_FRAME has no per-lane delay, so its board skew can land the frame "
                "edge one capture cycle away from the data lanes at the doubled DATA_CLK - a "
                "whole-slot rotation of the assembled words that the per-lane deskew metric "
                "cannot see. The bring-up sweeps 0..3 against the sequence-level PRBS verify."),
        ])
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

        # Clocking ---------------------------------------------------------------------------------

        # RX: DATA_CLK from the AD9361 drives the rfic domain (RX capture and the TX fabric side).
        # Relies on the AD9361's internal alignment for the common clock/data phase; the per-lane
        # residue is deskewed in the RX datapath below.
        rx_clk_ibufds = Signal()
        self.specials += [
            Instance("IBUFDS",
                i_I  = pads.rx_clk_p,
                i_IB = pads.rx_clk_n,
                o_O  = rx_clk_ibufds
            ),
            Instance("BUFG",
                i_I = rx_clk_ibufds,
                o_O = ClockSignal("rfic")
            ),
            AsyncResetSynchronizer(ClockDomain("rfic"), ResetSignal("sys")),
        ]

        # TX: serializer clocks from a dedicated MMCM off the DATA_CLK IBUFDS - CLK = 491.52 MHz
        # (DDR bit clock -> 983 Mbps) and CLKDIV = 122.88 MHz (8:1 parallel-load clock), both
        # BUFG'd and phase-coherent. The MMCM only locks at the 491.52 MHz DATA_CLK
        # (2T2R @ 122.88), so this PHY transmits only at that rate. No per-lane phasing (shared
        # CLK).
        self.tx_oserdes_mmcm = S7MMCM(speedgrade=-3)
        self.tx_oserdes_mmcm.register_clkin(rx_clk_ibufds, 491.52e6)
        self.clock_domains.cd_oserdes     = ClockDomain(reset_less=True)  # 491.52 CLK.
        self.clock_domains.cd_oserdes_div = ClockDomain(reset_less=True)  # 122.88 CLKDIV.
        self.tx_oserdes_mmcm.create_clkout(self.cd_oserdes,     491.52e6, phase=0, with_reset=False)
        self.tx_oserdes_mmcm.create_clkout(self.cd_oserdes_div, 122.88e6, phase=0, with_reset=False)

        # Clk measurement: at 491.52 MHz a 64-bit free-running counter does not close timing, so
        # expose a /4 divided rfic clock with increment=4 for MultiClkMeasurement (same reported
        # value, +/-4 cycles granularity).
        rfic_meas_div = Signal(2)
        self.sync.rfic += rfic_meas_div.eq(rfic_meas_div + 1)
        self.meas_clk = (rfic_meas_div[1], 4)

        # Control Path -----------------------------------------------------------------------------

        # CSR fields -> rfic-domain control signals.
        mode            = Signal() # Mode selection (1R1T or 2R2T).
        rx_frame_offset = Signal(2) # RX_FRAME-vs-data slot rotation (2R2T slot decode).
        self.specials += [
            MultiReg(self.control.fields.mode,            mode,            odomain="rfic"),
            MultiReg(self.control.fields.rx_frame_offset, rx_frame_offset, odomain="rfic"),
        ]

        # Per-lane IDELAYE2 tap loading (sys domain, consumed by the RX datapath): the CSR storage
        # fields update at the end of the write cycle, so the load strobes one cycle later
        # (idelay_re_d) when value/lane already hold the written values.
        rx_idelay_value = Signal(5)
        rx_idelay_ld    = Signal(6)
        idelay_re_d     = Signal()
        self.sync += idelay_re_d.eq(self.rx_deskew_idelay.re)
        self.comb += rx_idelay_value.eq(self.rx_deskew_idelay.fields.value)
        self.comb += [rx_idelay_ld[i].eq(idelay_re_d & (self.rx_deskew_idelay.fields.lane == i))
            for i in range(6)]

        # Per-lane deskew error counters: count lane_err events (driven by the RX datapath below),
        # latched/cleared together on the rx_deskew_ctrl.latch pulse and read back through the
        # rx_deskew_err CSRs. hold is stable between latches; sample it through a MultiReg.
        lane_err   = Signal(6)
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
            self.specials += MultiReg(hold, self.rx_deskew_err[i].status)

        # RX Datapath ------------------------------------------------------------------------------

        # RX Framing.
        # -----------
        # Processes RX_FRAME signal: high for MSBs (or channel 1 in 2R2T), low for LSBs(or channel 2
        # in 2R2T). Operates in SDR with 50% duty cycle.
        rx_frame_ibufds = Signal()
        rx_frame        = Signal()
        self.specials += [
            Instance("IBUFDS",
                i_I  = pads.rx_frame_p,
                i_IB = pads.rx_frame_n,
                o_O  = rx_frame_ibufds
            ),
            Instance("IDDR",
                p_DDR_CLK_EDGE = "SAME_EDGE_PIPELINED",
                i_C  = ClockSignal("rfic"),
                i_CE = 1,
                i_S  = 0,
                i_R  = 0,
                i_D  = rx_frame_ibufds,
                o_Q1 = rx_frame,
                o_Q2 = Open(),
            )
        ]

        # RX Counter.
        # -----------
        # Maintains a free-running counter, reset on RX_FRAME rising edge for synchronization; in
        # 2R2T the reset value carries the rx_frame_offset slot rotation (see the CSR field).
        rx_count   = Signal(2)
        rx_frame_d = Signal()
        self.sync.rfic += [
            rx_frame_d.eq(rx_frame),
            rx_count.eq(rx_count + 1),
            If(rx_frame & ~rx_frame_d,
                Case(mode, {
                    AD9361PHY1R1T_MODE: rx_count[0].eq(1),                   # Align for 1R1T.
                    AD9361PHY2R2T_MODE: rx_count   .eq(1 + rx_frame_offset), # Align for 2R2T
                                                                             # (+ slot rotation).
                })
            )
        ]

        # RX Data.
        # --------
        # Captures RX_D[5:0] in DDR mode through a per-lane IDELAYE2 (VAR_LOAD, loaded from the
        # control path above): I samples on rising edge, Q samples on falling edge. Assembles
        # 12-bit I/Q samples with MSBs first, then LSBs, interleaved.
        rx_data_ibufds = Signal(6)
        rx_data_in     = Signal(6)
        rx_data_half_i = Signal(6)
        rx_data_half_q = Signal(6)
        for i in range(6):
            self.specials += [
                Instance("IBUFDS",
                    i_I  = pads.rx_data_p[i],
                    i_IB = pads.rx_data_n[i],
                    o_O  = rx_data_ibufds[i]
                ),
                Instance("IDELAYE2",
                    p_IDELAY_TYPE           = "VAR_LOAD",
                    p_DELAY_SRC             = "IDATAIN",
                    p_REFCLK_FREQUENCY      = 200.0,
                    p_SIGNAL_PATTERN        = "DATA",
                    p_HIGH_PERFORMANCE_MODE = "TRUE",
                    p_PIPE_SEL              = "FALSE",
                    p_CINVCTRL_SEL          = "FALSE",
                    p_IDELAY_VALUE          = 0,
                    i_C           = ClockSignal("sys"),
                    i_LD          = rx_idelay_ld[i],
                    i_CNTVALUEIN  = rx_idelay_value,
                    i_IDATAIN     = rx_data_ibufds[i],
                    o_DATAOUT     = rx_data_in[i],
                    i_CE          = 0,
                    i_INC         = 0,
                    i_LDPIPEEN    = 0,
                    i_REGRST      = 0,
                    i_CINVCTRL    = 0,
                    i_DATAIN      = 0,
                    o_CNTVALUEOUT = Open(),
                ),
                Instance("IDDR",
                    p_DDR_CLK_EDGE = "SAME_EDGE_PIPELINED",
                    i_C  = ClockSignal("rfic"),
                    i_CE = 1,
                    i_S  = 0,
                    i_R  = 0,
                    i_D  = rx_data_in[i],
                    o_Q1 = rx_data_half_i[i],
                    o_Q2 = rx_data_half_q[i],
                )
            ]
        rx_data_ia   = Signal(12)
        rx_data_qa   = Signal(12)
        rx_data_ib   = Signal(12)
        rx_data_qb   = Signal(12)
        rx_valid     = Signal()
        rx_source_ia = Signal(12)
        rx_source_qa = Signal(12)
        rx_source_ib = Signal(12)
        rx_source_qb = Signal(12)
        self.sync.rfic += [
            Case(rx_count[1], {
                0b0: [  # Assemble IA/QA: MSBs in high bits, LSBs in low bits.
                    rx_data_ia[0: 6].eq(rx_data_half_i),
                    rx_data_ia[6:12].eq(rx_data_ia[0:6]),
                    rx_data_qa[0: 6].eq(rx_data_half_q),
                    rx_data_qa[6:12].eq(rx_data_qa[0:6]),
                ],
                0b1: [  # Assemble IB/QB: MSBs in high bits, LSBs in low bits.
                    rx_data_ib[0: 6].eq(rx_data_half_i),
                    rx_data_ib[6:12].eq(rx_data_ib[0:6]),
                    rx_data_qb[0: 6].eq(rx_data_half_q),
                    rx_data_qb[6:12].eq(rx_data_qb[0:6]),
                ]
            })
        ]

        # RX Source Interface.
        # --------------------
        # Outputs assembled RX samples when valid (rx_count == 0). The source is free-running:
        # valid pulses for a single cycle per sample set and is not held under backpressure, so
        # samples are dropped when the downstream stalls (overflow accounting happens at the DMA
        # level).
        self.sync.rfic += [
            rx_valid.eq(0),
            If(rx_count == 0,
                rx_valid.eq(1),
                rx_source_ia.eq(rx_data_ia),
                rx_source_qa.eq(rx_data_qa),
                rx_source_ib.eq(rx_data_ib),
                rx_source_qb.eq(rx_data_qb),
            )
        ]
        self.comb += [
            source.valid.eq(rx_valid),
            source.ia.eq(rx_source_ia),
            source.qa.eq(rx_source_qa),
            source.ib.eq(rx_source_ib),
            source.qb.eq(rx_source_qb),
        ]

        # RX Deskew Measurement.
        # ----------------------
        # Compare the channel 1 and channel 2 sample slots: with the AD9361 BIST PRBS enabled in
        # 2R2T mode the chip sends the same word in both slots, so any ch1/ch2 difference on a
        # lane's two word bits is a capture error on that lane (counted in the control path).
        # Pipelined for 491.52 MHz DATA_CLK.
        valid_r = Signal()
        ia_r    = Signal(12)
        qa_r    = Signal(12)
        ib_r    = Signal(12)
        qb_r    = Signal(12)
        self.sync.rfic += [
            valid_r.eq(source.valid),
            If(source.valid,
                ia_r.eq(source.ia),
                qa_r.eq(source.qa),
                ib_r.eq(source.ib),
                qb_r.eq(source.qb),
            )
        ]
        for i in range(6):
            self.sync.rfic += lane_err[i].eq(valid_r & (
                (ia_r[i] ^ ib_r[i]) | (ia_r[i+6] ^ ib_r[i+6]) |
                (qa_r[i] ^ qb_r[i]) | (qa_r[i+6] ^ qb_r[i+6])
            ))

        # TX Datapath ------------------------------------------------------------------------------

        # TX Sink Interface.
        # ------------------
        # Accepts new TX samples just before the serializer wraps to count 0, so the next word
        # starts with the freshly latched IA/QA MSBs.
        tx_count = Signal(2)
        self.sync.rfic += tx_count.eq(tx_count + 1)
        self.comb += sink.ready.eq(tx_count == 3)

        # TX Data Latching.
        # -----------------
        # Latches TX samples when ready; defaults to 0 if invalid to avoid spurs.
        tx_data_ia = Signal(12)
        tx_data_qa = Signal(12)
        tx_data_ib = Signal(12)
        tx_data_qb = Signal(12)
        self.sync.rfic += [
            If(sink.ready,
                # Clear to avoid spurs.
                tx_data_ia.eq(0),
                tx_data_qa.eq(0),
                tx_data_ib.eq(0),
                tx_data_qb.eq(0),
                If(sink.valid,
                    # Latch valid samples.
                    tx_data_ia.eq(sink.ia),
                    tx_data_qa.eq(sink.qa),
                    tx_data_ib.eq(sink.ib),
                    tx_data_qb.eq(sink.qb),
                )
            )
        ]

        # TX Group Hand-Off (rfic -> CLKDIV).
        # -----------------------------------
        # The TX sample groups cross rfic (491.52, BUFG of DATA_CLK) -> OSERDES CLKDIV (122.88,
        # MMCM) through a small registered-flags gray-pointer async FIFO. The two domains run at a
        # fixed 4:1 ratio off the same DATA_CLK, but their relative phase is arbitrary: the
        # group-update phase follows tx_count (free-running, re-rolls with every rfic clock
        # restart) and the CLKDIV /4 phase follows the MMCM lock. A direct re-registration of the
        # group in CLKDIV therefore lands its capture edge inside the group's update window on
        # some lock/count alignments and tears the samples; the FIFO's gray-pointer handshake
        # makes the transfer phase-agnostic, so every timing-met build and every MMCM lock
        # transfers whole groups. FF storage (a 491.52 MHz LUTRAM write clock would violate the
        # RAMD32 min low-pulse-width); depth 8 so the write side's conservative
        # (pointer-sync-delayed) full flag stays clear at the steady ~3-deep occupancy. Both
        # pointer domains share one reset (rfic reset or MMCM unlock, async-synchronized into
        # each side) so the pointers can never desynchronize; the whole-group pipeline latency is
        # invisible to the AD9361 (TX_FRAME repeats identically per group) and is absorbed by the
        # interface alignment + tx_offset calibration.
        self.clock_domains.cd_oserdes_wr = ClockDomain()  # rfic clock, FIFO write side.
        self.clock_domains.cd_oserdes_rd = ClockDomain()  # CLKDIV clock, FIFO read side.
        tx_group_fifo_rst = Signal()
        self.comb += [
            self.cd_oserdes_wr.clk.eq(ClockSignal("rfic")),
            self.cd_oserdes_rd.clk.eq(ClockSignal("oserdes_div")),
            tx_group_fifo_rst.eq(ResetSignal("rfic") | ~self.tx_oserdes_mmcm.locked),
        ]
        self.specials += [
            AsyncResetSynchronizer(self.cd_oserdes_wr, tx_group_fifo_rst),
            AsyncResetSynchronizer(self.cd_oserdes_rd, tx_group_fifo_rst),
        ]
        tx_group_fifo = AsyncFIFORegistered(width=48, depth=8, register_storage=True,
                                            registered_write=True)
        tx_group_fifo = ClockDomainsRenamer({"write": "oserdes_wr", "read": "oserdes_rd"})(tx_group_fifo)
        self.submodules.tx_group_fifo = tx_group_fifo
        # tx_data_* hold the newly latched group from the cycle after sink.ready; write it then.
        tx_group_we = Signal()
        self.sync.rfic += tx_group_we.eq(sink.ready)
        self.comb += [
            tx_group_fifo.din.eq(Cat(tx_data_ia, tx_data_qa, tx_data_ib, tx_data_qb)),
            tx_group_fifo.we.eq(tx_group_we),
            tx_group_fifo.re.eq(1),
        ]
        ia_d = Signal(12)
        qa_d = Signal(12)
        ib_d = Signal(12)
        qb_d = Signal(12)
        self.sync.oserdes_div += [
            # Air zeros until a group is available (startup / MMCM relock) to avoid spurs.
            If(tx_group_fifo.readable,
                Cat(ia_d, qa_d, ib_d, qb_d).eq(tx_group_fifo.dout),
            ).Else(
                Cat(ia_d, qa_d, ib_d, qb_d).eq(0),
            )
        ]

        # TX Serializer Reset.
        # --------------------
        # Shared CLKDIV-synchronous OSERDES reset: OSERDESE2's internal 8:1 load phase is
        # established when RST deasserts, so ALL instances (6 data lanes + TX_FRAME) must leave
        # reset in the SAME CLKDIV cycle - a raw async reset lets each instance release on a
        # different CLKDIV edge and come up serialized with a whole-bit-cycle offset relative to
        # the other lanes (a per-build routing lottery: the affected lane shows a deterministic
        # 100% error rate that no chip delay tap can fix, while the other lanes are clean). One
        # 2-FF synchronizer in the CLKDIV domain drives every RST pin, and the register -> RST
        # nets are normally timed (single CLKDIV cycle), so every timing-met build releases all
        # serializers in lockstep. Held while the MMCM is unlocked.
        oserdes_rst_meta = Signal(reset=1)
        oserdes_rst      = Signal(reset=1)
        self.sync.oserdes_div += [
            oserdes_rst_meta.eq(ResetSignal("sys") | ~self.tx_oserdes_mmcm.locked),
            oserdes_rst.eq(oserdes_rst_meta),
        ]

        # TX Clocking Output.
        # -------------------
        # FB_CLK to the AD9361 (same frequency as DATA_CLK), from the serializer MMCM so it
        # presents the same launch phase as the OSERDES outputs.
        tx_clk_obufds = Signal()
        self.specials += [
            Instance("ODDR",
                p_DDR_CLK_EDGE = "SAME_EDGE",
                i_C  = ClockSignal("oserdes"),
                i_CE = 1,
                i_S  = 0,
                i_R  = 0,
                i_D1 = 1,
                i_D2 = 0,
                o_Q  = tx_clk_obufds,
            ),
            Instance("OBUFDS",
                i_I  = tx_clk_obufds,
                o_O  = pads.tx_clk_p,
                o_OB = pads.tx_clk_n
            )
        ]

        # TX Data/Frame Serializers.
        # --------------------------
        # 8:1 DDR OSERDES: per CLKDIV (122.88) cycle, load 8 half-cycle bits per lane; OSERDES
        # serializes them at CLK (491.52) DDR = 983 Mbps. D1 shifts out FIRST; the per-lane D1..D8
        # order below is bit-identical to what the simple PHY's ODDR serializer emits.
        tx_data_obufds  = Signal(6)
        tx_frame_obufds = Signal()

        def oserdes(d1, d2, d3, d4, d5, d6, d7, d8, oq):
            return Instance("OSERDESE2",
                p_DATA_RATE_OQ   = "DDR",
                p_DATA_RATE_TQ   = "SDR",
                p_DATA_WIDTH     = 8,
                p_TRISTATE_WIDTH = 1,
                p_SERDES_MODE    = "MASTER",
                i_CLK    = ClockSignal("oserdes"),
                i_CLKDIV = ClockSignal("oserdes_div"),
                i_OCE    = 1,
                i_RST    = oserdes_rst,
                i_TCE    = 0, i_T1=0, i_T2=0, i_T3=0, i_T4=0, i_TBYTEIN=0,
                i_SHIFTIN1=0, i_SHIFTIN2=0,
                i_D1=d1, i_D2=d2, i_D3=d3, i_D4=d4, i_D5=d5, i_D6=d6, i_D7=d7, i_D8=d8,
                o_OQ=oq,
            )

        # Data lanes: D1..D8 = [IAmsb, QAmsb, IAlsb, QAlsb, IBmsb, QBmsb, IBlsb, QBlsb]_i.
        for i in range(6):
            self.specials += oserdes(
                ia_d[6+i], qa_d[6+i], ia_d[0+i], qa_d[0+i],
                ib_d[6+i], qb_d[6+i], ib_d[0+i], qb_d[0+i],
                tx_data_obufds[i])
            self.specials += Instance("OBUFDS",
                i_I  = tx_data_obufds[i],
                o_O  = pads.tx_data_p[i],
                o_OB = pads.tx_data_n[i]
            )
        # Frame: 2R2T = 4 high + 4 low half-cycles, aligned to the data by construction.
        self.specials += oserdes(1, 1, 1, 1, 0, 0, 0, 0, tx_frame_obufds)
        self.specials += Instance("OBUFDS",
            i_I  = tx_frame_obufds,
            o_O  = pads.tx_frame_p,
            o_OB = pads.tx_frame_n
        )

    def add_timing_constraints(self, platform):
        """Vivado timing exceptions for this PHY's internals (see AD9361RFIC for the datapath's).

        Every rfic (491.52 MHz, BUFG of DATA_CLK) <-> OSERDES-MMCM-clock (CLK/CLKDIV) crossing
        goes through the gray-pointer group FIFO (pointers via MultiReg, storage -> dout
        qualified by the handshake, resets via AsyncResetSynchronizer), so the pair is
        asynchronous like the design's other CDC clock pairs - a phase analysis of an
        MMCM-vs-BUFG pair of the same input pin would be wrong across MMCM locks anyway. Guarded
        like the platform's own clock-group commands (set_clock_groups errors on an empty group,
        and the generated-clock names depend on the LiteX netlist naming). The first stage of the
        shared OSERDES-reset synchronizer is an async capture of the sys reset / MMCM-lock
        condition into the CLKDIV domain.
        """
        platform.toolchain.pre_placement_commands += [
            "set _clk0 [get_clocks -quiet rfic_clk]; "
            "set _clk1 [get_clocks -quiet *ad9361oversamplingphy_clkout*]; "
            "if {{[llength $_clk0] && [llength $_clk1]}} {{ "
            "set_clock_groups -asynchronous -group $_clk0 -group $_clk1 }}",
            "set_false_path -quiet -to [get_cells -hierarchical "
            "-filter {{NAME =~ *oserdes_rst_meta*}}]",
        ]
