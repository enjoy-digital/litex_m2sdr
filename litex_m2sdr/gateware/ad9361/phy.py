#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litex.soc.cores.clock import S7MMCM

"""
AD9361 RFIC PHY (Dual Port Full Duplex LVDS Mode).

This module provides a physical layer interface for the AD9361 RFIC in Dual Port Full Duplex LVDS mode
on Xilinx 7-series FPGAs. It supports simultaneous RX and TX operations using separate Rx_D[5:0] and
Tx_D[5:0] buses. The module handles 1R1T (4-way interleaving) and 2R2T (8-way interleaving) modes.
For 2R1T or 1R2T configurations, use 2R2T mode and ignore unused slots.

No phase alignment is required in the PHY, as the AD9361's internal clock/data alignment capability,
combined with software calibration, ensures proper synchronization, eliminating the need for FPGA-based
phase alignment.

Key Features:
- Data format           : Two's complement, MSBs first, I/Q interleaved.
- 1R1T mode             : 'a' for first sample, 'b' for second; Interleave: I_MSB, Q_MSB, I_LSB, Q_LSB,...
- 2R2T mode             : 'a' for channel 1,    'b' for channel 2; Interleave: I1_MSB, Q1_MSB, I1_LSB, Q1_LSB, I2_MSB,...
- Configuration via CSR : `mode` (1R1T/2R2T) and `loopback` (enable/disable).
- No phase alignment required between DATA_CLK (RX clock) and FB_CLK (TX clock).

"""

# Constants ----------------------------------------------------------------------------------------

AD9361PHY2R2T_MODE = 0
AD9361PHY1R1T_MODE = 1

def phy_layout():
    """Defines 12-bit I/Q sample layout for AD9361 PHY channels 'a' and 'b'."""
    return stream.EndpointDescription([
        ("ia", 12),  # I samples for channel/sample 'a'.
        ("qa", 12),  # Q samples for channel/sample 'a'.
        ("ib", 12),  # I samples for channel/sample 'b'.
        ("qb", 12),  # Q samples for channel/sample 'b'.
    ])

# AD9361PHY ----------------------------------------------------------------------------------------

class AD9361PHY(LiteXModule):
    """
    AD9361 RFIC PHY Module.

    Manages the physical layer interface for the AD9361 RFIC, handling RX and TX data streams
    in Dual Port Full Duplex LVDS mode. Leverages AD9361's clock/data alignment and software
    calibration to avoid FPGA-based phase alignment.

    Parameters:
        pads: External FPGA pins connected to the AD9361.
        with_loopback: Include the internal TX->RX loopback path (FIFO + muxes). Disable to relax
            rfic_clk timing at high DATA_CLK rates (491.52MHz with Oversampling); the `loopback`
            CSR field then has no effect.
        with_rx_idelay: Insert an IDELAYE2 on each RX data lane. The AD9361's RX clock/data delay
            registers shift all lanes together; at 983Mbps per lane (2T2R with Oversampling) the
            board's ~300ps lane-to-lane skew leaves no common eye, so per-lane deskew is required.
            Drive rx_idelay_value/rx_idelay_ld from the sys domain to load per-lane tap values
            (requires an IDELAYCTRL with a 200MHz reference in the design).
        with_tx_phase: Clock each TX data lane's ODDR (and the FB_CLK ODDR) from an MMCM-phased
            copy of rfic_clk instead of rfic_clk directly, selectable at runtime. The TX
            direction has no per-lane delay primitive in Artix-7 HR banks; the MMCM's per-output
            static phase (re-programmable through its DRP, VCO/8 steps) provides the per-lane
            trim that aligns the 983Mbps TX eyes at 491.52MHz DATA_CLK. FB_CLK's ODDR drives a
            constant pattern, so its output can be phased across the full bit period (the global
            search axis); the data lanes accept small forward trims. The MMCM only locks at the
            491.52MHz DATA_CLK; at other rates leave the bypass selected (ODDRs on rfic_clk).
    """
    def __init__(self, pads, with_loopback=True, with_rx_idelay=False,
                 with_tx_phase=False, with_tx_oserdes=False):
        # with_tx_oserdes: drive the 6 TX-data lanes + TX_FRAME from OSERDESE2 (8:1 DDR, CLK 491.52 /
        # CLKDIV 122.88) instead of fabric-clocked ODDRs. At 2T2R@122.88 the TX LVDS runs 983Mbps/lane
        # and the ODDR output eye is too poor for the chip to de-interleave (a clean tone splatters to
        # fs/2); OSERDESE2 gives a clean eye and the datapath fabric runs at 122.88 (the CLKDIV). Uses
        # a single shared clock for all lanes; the MMCM only locks at the 491.52 DATA_CLK, so an
        # OSERDES build supports TX only at 2T2R@122.88. Mutually exclusive with with_tx_phase (both
        # build the TX MMCM).
        assert not (with_tx_oserdes and with_tx_phase), "with_tx_oserdes and with_tx_phase are exclusive"
        self.sink    = sink   = stream.Endpoint(phy_layout()) # TX input  stream.
        self.source  = source = stream.Endpoint(phy_layout()) # RX output stream.
        self.control = CSRStorage(fields=[
            CSRField("mode", size=1, offset=0, values=[
                ("``0b0``", "2R2T mode (8-way interleaving)"),
                ("``0b1``", "1R1T mode (4-way interleaving)"),
            ], description="Selects between 1R1T and 2R2T modes"),
            CSRField("loopback", size=1, offset=1, values=[
                ("``0b0``", "Normal operation (no loopback)"),
                ("``0b1``", "Loopback TX data to RX internally"),
            ], description="Enables/disables internal loopback mode"),
            CSRField("tx_frame_offset", size=2, offset=2, reset=0, description=
                "Rotate TX_FRAME by N DATA_CLK cycles relative to the TX data slots. "
                "At the doubled DATA_CLK (491.52MHz, 2R2T Oversampling) the chip's "
                "channel/IQ de-interleave locks to TX_FRAME; the data-lane PRBS "
                "calibration is slot-symmetric and cannot pin this pairing, so the "
                "correct rotation (0..3) is selected here."),
        ])

        if with_rx_idelay:
            self.rx_idelay_value = Signal(5) # Tap value to load (sys domain).
            self.rx_idelay_ld    = Signal(6) # Per-lane load strobe (sys domain).

        # # #

        # Control Signals
        # ---------------
        mode = Signal()  # Mode selection (1R1T or 2R2T).
        self.specials += MultiReg(self.control.fields.mode, mode, odomain="rfic")
        tx_frame_offset = Signal(2)  # TX_FRAME-vs-data slot rotation (2R2T de-interleave).
        self.specials += MultiReg(self.control.fields.tx_frame_offset, tx_frame_offset, odomain="rfic")
        if with_loopback:
            loopback = Signal()  # Loopback enable/disable.
            self.specials += MultiReg(self.control.fields.loopback, loopback, odomain="rfic")

        # RX PHY -----------------------------------------------------------------------------------

        # RX Clocking.
        # ------------
        # Uses DATA_CLK from AD9361 to clock RX data and frame signals. Relies on AD9361's internal
        # alignment, no FPGA phase adjustment needed.
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

        # RX Counter
        # ----------
        # Maintains a free-running counter, reset to 1 on RX_FRAME rising edge for synchronization.
        rx_count   = Signal(2)
        rx_frame_d = Signal()
        self.sync.rfic += [
            rx_frame_d.eq(rx_frame),
            rx_count.eq(rx_count + 1),
            If(rx_frame & ~rx_frame_d,
                Case(mode, {
                    AD9361PHY1R1T_MODE: rx_count[0].eq(1), # Align for 1R1T.
                    AD9361PHY2R2T_MODE: rx_count   .eq(1), # Align for 2R2T.
                })
            )
        ]

        # RX Data.
        # --------
        # Captures RX_D[5:0] in DDR mode: I samples on rising edge, Q samples on falling edge.
        # Assembles 12-bit I/Q samples with MSBs first, then LSBs, interleaved. Uses AD9361's
        # alignment to ensure correct data capture.
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
            if with_rx_idelay:
                self.specials += Instance("IDELAYE2",
                    p_IDELAY_TYPE           = "VAR_LOAD",
                    p_DELAY_SRC             = "IDATAIN",
                    p_REFCLK_FREQUENCY      = 200.0,
                    p_SIGNAL_PATTERN        = "DATA",
                    p_HIGH_PERFORMANCE_MODE = "TRUE",
                    p_PIPE_SEL              = "FALSE",
                    p_CINVCTRL_SEL          = "FALSE",
                    p_IDELAY_VALUE          = 0,
                    i_C           = ClockSignal("sys"),
                    i_LD          = self.rx_idelay_ld[i],
                    i_CNTVALUEIN  = self.rx_idelay_value,
                    i_IDATAIN     = rx_data_ibufds[i],
                    o_DATAOUT     = rx_data_in[i],
                    i_CE          = 0,
                    i_INC         = 0,
                    i_LDPIPEEN    = 0,
                    i_REGRST      = 0,
                    i_CINVCTRL    = 0,
                    i_DATAIN      = 0,
                    o_CNTVALUEOUT = Open(),
                )
            else:
                self.comb += rx_data_in[i].eq(rx_data_ibufds[i])
        rx_data_ia  = Signal(12)
        rx_data_qa  = Signal(12)
        rx_data_ib  = Signal(12)
        rx_data_qb  = Signal(12)
        rx_valid    = Signal()
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
        # Outputs assembled RX samples when valid (rx_count == 0). The source
        # is free-running: valid pulses for a single cycle per sample set and
        # is not held under backpressure, so samples are dropped when the
        # downstream stalls (overflow accounting happens at the DMA level).
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

        # TX -> RX Loopback ------------------------------------------------------------------------

        # Routes TX data to RX when loopback is enabled. Minimal FF-based depth-1 skid (NOT a
        # SyncFIFO): a SyncFIFO stores in distributed RAM whose write clock at 491.52MHz rfic_clk
        # violates the RAMD32 min low-pulse-width (the same limit that forced register storage on
        # the RX CDC), which is what broke timing here. A single registered group + valid flag is
        # enough because TX and RX both move one group per 4 rfic_clk cycles. The handshake/capture
        # is completed in the TX section below (it needs tx_count).
        rx_source_connect = [
            source.valid.eq(rx_valid),
            source.ia.eq(rx_source_ia),
            source.qa.eq(rx_source_qa),
            source.ib.eq(rx_source_ib),
            source.qb.eq(rx_source_qb),
        ]
        if with_loopback:
            lb_valid = Signal()
            lb_ia = Signal(12); lb_qa = Signal(12); lb_ib = Signal(12); lb_qb = Signal(12)
            self.comb += [
                If(loopback,
                    source.valid.eq(lb_valid),
                    source.ia.eq(lb_ia),
                    source.qa.eq(lb_qa),
                    source.ib.eq(lb_ib),
                    source.qb.eq(lb_qb),
                ).Else(
                    *rx_source_connect,
                )
            ]
        else:
            self.comb += rx_source_connect

        # TX PHY -----------------------------------------------------------------------------------

        # TX Sink Interface.
        # ------------------
        # Accepts new TX samples just before the serializer wraps to count 0,
        # so the next word starts with the freshly latched IA/QA MSBs.
        tx_count = Signal(2)
        self.sync.rfic += tx_count.eq(tx_count + 1)
        if with_loopback:
            # Capture a TX group into the skid when one is consumed (tx_count==3) and the skid can
            # accept it (empty, or being drained this cycle). Drains into the RX source above.
            # The capture enable uses a dedicated tx_count replica (tx_count_lb): tx_count itself
            # is IOB-anchored (it drives the ODDR serializer) so the route from it to the fabric
            # lb registers is long; a kept local copy places next to the lb registers -> short route
            # at 491.52MHz. Both counters reset together and increment every cycle so they match.
            tx_count_lb = Signal(2)
            tx_count_lb.attr.add(("dont_touch", "true"))
            self.sync.rfic += tx_count_lb.eq(tx_count_lb + 1)
            lb_we = Signal()
            self.comb += [
                sink.ready.eq((tx_count == 3) & (~loopback | ~lb_valid | source.ready)),
                lb_we.eq(loopback & sink.valid & (tx_count_lb == 3) & (~lb_valid | source.ready)),
            ]
            self.sync.rfic += [
                If(lb_we,
                    lb_ia.eq(sink.ia), lb_qa.eq(sink.qa), lb_ib.eq(sink.ib), lb_qb.eq(sink.qb),
                    lb_valid.eq(1),
                ).Elif(source.ready,
                    lb_valid.eq(0),
                )
            ]
        else:
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

        # TX Phase Trim (Oversampling).
        # -----------------------------
        # MMCM-phased copies of rfic_clk for the TX ODDRs, with a per-output glitching async
        # bypass mux back to rfic_clk (used whenever DATA_CLK is not 491.52MHz and the MMCM is
        # unlocked). Phases are static (0 at build, analyzed as such) and re-programmed at
        # runtime through the DRP; data-lane trims stay small and forward so the rfic-domain
        # launch registers keep setup margin into the phased ODDRs.
        if with_tx_phase:
            self.tx_phase = CSRStorage(fields=[
                CSRField("en", size=1, offset=0,
                    description="Clock the TX ODDRs from the MMCM-phased clocks."),
                CSRField("mmcm_reset", size=1, offset=1,
                    description="Hold the TX MMCM in reset (assert around DRP phase writes)."),
            ])
            self.tx_mmcm = S7MMCM(speedgrade=-3)
            # Feed the MMCM and the bypass inputs from the IBUFDS output (dedicated routes);
            # cascading out of the rfic BUFG is not placeable.
            self.tx_mmcm.register_clkin(rx_clk_ibufds, 491.52e6)
            self.comb += self.tx_mmcm.reset.eq(self.tx_phase.fields.mmcm_reset)
            tx_ph_cds = []
            for i in range(7):
                cd_raw = ClockDomain(f"rfic_txphraw{i}", reset_less=True)
                self.clock_domains += cd_raw
                self.tx_mmcm.create_clkout(cd_raw, 491.52e6, phase=0, buf=None, with_reset=False)
                cd_mux = ClockDomain(f"rfic_txph{i}", reset_less=True)
                self.clock_domains += cd_mux
                self.specials += Instance("BUFGCTRL",
                    i_I0      = rx_clk_ibufds,
                    i_I1      = cd_raw.clk,
                    i_S0      = ~self.tx_phase.fields.en,
                    i_S1      = self.tx_phase.fields.en,
                    i_CE0     = 1,
                    i_CE1     = 1,
                    i_IGNORE0 = 1,
                    i_IGNORE1 = 1,
                    o_O       = cd_mux.clk,
                )
                tx_ph_cds.append(cd_mux)
            self.tx_mmcm.expose_drp()
            tx_lane_clk = [cd.clk for cd in tx_ph_cds[:6]]
            tx_fb_clk   = tx_ph_cds[6].clk
        elif with_tx_oserdes:
            # OSERDES clocking: CLK=491.52MHz (DDR bit clock -> 983Mbps), CLKDIV=122.88MHz (8:1
            # parallel-load clock). A dedicated MMCM off the DATA_CLK IBUFDS; both clocks are BUFG'd
            # and phase-coherent. The MMCM only locks at the 491.52 DATA_CLK (2T2R@122.88), so an
            # OSERDES build supports TX only at that rate. No per-lane phasing (shared CLK).
            self.tx_oserdes_mmcm = S7MMCM(speedgrade=-3)
            self.tx_oserdes_mmcm.register_clkin(rx_clk_ibufds, 491.52e6)
            self.clock_domains.cd_oserdes     = ClockDomain(reset_less=True)  # 491.52 CLK.
            self.clock_domains.cd_oserdes_div = ClockDomain(reset_less=True)  # 122.88 CLKDIV.
            self.tx_oserdes_mmcm.create_clkout(self.cd_oserdes,     491.52e6, phase=0, with_reset=False)
            self.tx_oserdes_mmcm.create_clkout(self.cd_oserdes_div, 122.88e6, phase=0, with_reset=False)
            tx_lane_clk = [ClockSignal("oserdes") for _ in range(6)]
            tx_fb_clk   = ClockSignal("oserdes")
        else:
            tx_lane_clk = [ClockSignal("rfic") for _ in range(6)]
            tx_fb_clk   = ClockSignal("rfic")

        # TX Clocking.
        # ------------
        # Output: FB_CLK to AD9361, derived from RFIC clock (same frequency as DATA_CLK).
        tx_clk_obufds = Signal()
        self.specials += [
            Instance("ODDR",
                p_DDR_CLK_EDGE = "SAME_EDGE",
                i_C  = tx_fb_clk,
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


        # TX Framing
        # ----------
        # TX_FRAME marks the interleave: high for channel-1 slots, low for channel-2 (2R2T; MSB/LSB
        # in 1R1T). SDR, 50% duty. Registered once in the rfic domain before the ODDR to keep the
        # mux off the IOB path at 491.52MHz; frame and data get the same pipeline delay so their
        # alignment is preserved. tx_frame_offset rotates the frame vs the data slots (2R2T
        # de-interleave alignment, see the CSR field). With TX phase trim the frame's final register
        # and ODDR ride lane 0's phased clock, so the frame presents the same launch-to-FB_CLK phase
        # as the data (else the chip samples it in the wrong DATA_CLK slot). Skipped for the OSERDES
        # serializer, which drives TX_FRAME from its own OSERDESE2 in the block below.
        tx_frame   = Signal()
        tx_frame_r = Signal()
        tx_frame_r2 = Signal()
        tx_frame_obufds = Signal()
        tx_count_f = Signal(2)
        self.comb += tx_count_f.eq(tx_count + tx_frame_offset)
        self.comb += [
            If(mode == AD9361PHY1R1T_MODE,
                Case(tx_count_f, {  # 1R1T: 4-way interleaving over 2 clocks.
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(0),
                    0b10 : tx_frame.eq(1),
                    0b11 : tx_frame.eq(0),
                })
            ).Elif(mode == AD9361PHY2R2T_MODE,
                Case(tx_count_f, {  # 2R2T: 8-way interleaving over 4 clocks.
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(1),
                    0b10 : tx_frame.eq(0),
                    0b11 : tx_frame.eq(0),
                })
            )
        ]
        if with_tx_phase:
            frame_sync = self.sync.rfic_txph0
        else:
            frame_sync = self.sync.rfic
        self.sync.rfic += tx_frame_r.eq(tx_frame)
        frame_sync += tx_frame_r2.eq(tx_frame_r)
        if not with_tx_oserdes:
            self.specials += Instance("ODDR",
                p_DDR_CLK_EDGE = "SAME_EDGE",
                i_C  = tx_lane_clk[0],
                i_CE = 1,
                i_S  = 0,
                i_R  = 0,
                i_D1 = tx_frame_r2,
                i_D2 = tx_frame_r2,
                o_Q  = tx_frame_obufds,
            )
        self.specials += Instance("OBUFDS",
            i_I  = tx_frame_obufds,
            o_O  = pads.tx_frame_p,
            o_OB = pads.tx_frame_n
        )

        # TX Data
        # -------
        # Outputs TX_D[5:0] in DDR mode: I samples on rising edge, Q samples on falling edge.
        # Sequences MSBs first, then LSBs; I and Q interleaved. Relies on AD9361's alignment for
        # correct data transmission.
        tx_data_half_i   = Signal(6)
        tx_data_half_q   = Signal(6)
        tx_data_half_i_r = Signal(6)
        tx_data_half_q_r = Signal(6)
        tx_data_obufds   = Signal(6)
        self.comb += [
            Case(tx_count, {
                0b00: [  # IA/QA MSBs.
                    tx_data_half_i.eq(tx_data_ia[6:12]),
                    tx_data_half_q.eq(tx_data_qa[6:12]),
                ],
                0b01: [  # IA/QA LSBs.
                    tx_data_half_i.eq(tx_data_ia[0:6]),
                    tx_data_half_q.eq(tx_data_qa[0:6]),
                ],
                0b10: [  # IB/QB MSBs.
                    tx_data_half_i.eq(tx_data_ib[6:12]),
                    tx_data_half_q.eq(tx_data_qb[6:12]),
                ],
                0b11: [  # IB/QB LSBs.
                    tx_data_half_i.eq(tx_data_ib[0:6]),
                    tx_data_half_q.eq(tx_data_qb[0:6]),
                ]
            })
        ]
        self.sync.rfic += [
            tx_data_half_i_r.eq(tx_data_half_i),
            tx_data_half_q_r.eq(tx_data_half_q),
        ]
        tx_data_half_i_r2 = Signal(6)
        tx_data_half_q_r2 = Signal(6)
        for i in range(6):
            if with_tx_phase:
                lane_sync = getattr(self.sync, f"rfic_txph{i}")
            else:
                lane_sync = self.sync.rfic
            lane_sync += [
                tx_data_half_i_r2[i].eq(tx_data_half_i_r[i]),
                tx_data_half_q_r2[i].eq(tx_data_half_q_r[i]),
            ]
            if not with_tx_oserdes:
                self.specials += [
                    Instance("ODDR",
                        p_DDR_CLK_EDGE = "SAME_EDGE",
                        i_C  = tx_lane_clk[i],
                        i_CE = 1,
                        i_S  = 0,
                        i_R  = 0,
                        i_D1 = tx_data_half_i_r2[i],
                        i_D2 = tx_data_half_q_r2[i],
                        o_Q  = tx_data_obufds[i],
                    ),
                    Instance("OBUFDS",
                        i_I  = tx_data_obufds[i],
                        o_O  = pads.tx_data_p[i],
                        o_OB = pads.tx_data_n[i]
                    )
                ]

        # TX Data/Frame via OSERDESE2 -------------------------------------------------------------
        if with_tx_oserdes:
            # 8:1 DDR OSERDES: per CLKDIV(122.88) cycle, load 8 half-cycle bits per lane; OSERDES
            # serializes them at CLK(491.52) DDR = 983Mbps. D1 shifts out FIRST; the per-lane D1..D8
            # order below is bit-identical to what the ODDR serializer emits. The group
            # (tx_data_ia/qa/ib/qb, latched in rfic at tx_count==3, stable for one full CLKDIV
            # period) is re-registered into the CLKDIV domain (mesochronous: both clocks derive from
            # DATA_CLK; the rfic->oserdes_div crossing is constrained in litex_m2sdr.py).
            ia_d = Signal(12); qa_d = Signal(12); ib_d = Signal(12); qb_d = Signal(12)
            self.sync.oserdes_div += [
                ia_d.eq(tx_data_ia), qa_d.eq(tx_data_qa),
                ib_d.eq(tx_data_ib), qb_d.eq(tx_data_qb),
            ]
            def oserdes(d1,d2,d3,d4,d5,d6,d7,d8, oq):
                return Instance("OSERDESE2",
                    p_DATA_RATE_OQ   = "DDR",
                    p_DATA_RATE_TQ   = "SDR",
                    p_DATA_WIDTH     = 8,
                    p_TRISTATE_WIDTH = 1,
                    p_SERDES_MODE    = "MASTER",
                    i_CLK    = ClockSignal("oserdes"),
                    i_CLKDIV = ClockSignal("oserdes_div"),
                    i_OCE    = 1,
                    i_RST    = ResetSignal("sys"),
                    i_TCE    = 0, i_T1=0, i_T2=0, i_T3=0, i_T4=0, i_TBYTEIN=0,
                    i_SHIFTIN1=0, i_SHIFTIN2=0,
                    i_D1=d1, i_D2=d2, i_D3=d3, i_D4=d4, i_D5=d5, i_D6=d6, i_D7=d7, i_D8=d8,
                    o_OQ=oq,
                )
            # Data lanes: D1..D8 = hc0..hc7 = [IAmsb, QAmsb, IAlsb, QAlsb, IBmsb, QBmsb, IBlsb, QBlsb]_i
            for i in range(6):
                self.specials += oserdes(
                    ia_d[6+i], qa_d[6+i], ia_d[0+i], qa_d[0+i],
                    ib_d[6+i], qb_d[6+i], ib_d[0+i], qb_d[0+i],
                    tx_data_obufds[i])
                self.specials += Instance("OBUFDS",
                    i_I=tx_data_obufds[i], o_O=pads.tx_data_p[i], o_OB=pads.tx_data_n[i])
            # Frame: 2R2T = 4 high + 4 low half-cycles, aligned to the data by construction.
            self.specials += oserdes(1,1,1,1,0,0,0,0, tx_frame_obufds)
