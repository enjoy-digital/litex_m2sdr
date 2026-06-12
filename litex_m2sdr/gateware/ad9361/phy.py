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
    def __init__(self, pads, with_loopback=True, with_rx_idelay=False, with_tx_phase=False):
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
        ])

        if with_rx_idelay:
            self.rx_idelay_value = Signal(5) # Tap value to load (sys domain).
            self.rx_idelay_ld    = Signal(6) # Per-lane load strobe (sys domain).

        # # #

        # Control Signals
        # ---------------
        mode = Signal()  # Mode selection (1R1T or 2R2T).
        self.specials += MultiReg(self.control.fields.mode, mode, odomain="rfic")
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

        # Routes TX data to RX when loopback is enabled. Keep an RFIC-domain FIFO
        # here instead of a single registered word so TX serializer pacing is
        # decoupled from RX-side Ethernet/backpressure pauses during diagnostics.
        rx_source_connect = [
            source.valid.eq(rx_valid),
            source.ia.eq(rx_source_ia),
            source.qa.eq(rx_source_qa),
            source.ib.eq(rx_source_ib),
            source.qb.eq(rx_source_qb),
        ]
        if with_loopback:
            self.loopback_fifo = loopback_fifo = ClockDomainsRenamer("rfic")(
                stream.SyncFIFO(phy_layout(), depth=16, buffered=True)
            )
            self.comb += [
                If(loopback,
                    loopback_fifo.source.connect(source),
                ).Else(
                    loopback_fifo.source.ready.eq(1),
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
            self.comb += [
                sink.ready.eq((tx_count == 3) & (~loopback | loopback_fifo.sink.ready)),
                loopback_fifo.sink.valid.eq(loopback & sink.valid & (tx_count == 3)),
                loopback_fifo.sink.ia.eq(sink.ia),
                loopback_fifo.sink.qa.eq(sink.qa),
                loopback_fifo.sink.ib.eq(sink.ib),
                loopback_fifo.sink.qb.eq(sink.qb),
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
        # Generates TX_FRAME: high for MSBs (or channel 1 in 2R2T), low for LSBs(or channel 2 in
        # 2R2T). Uses SDR with 50% duty cycle.
        # TX_FRAME and TX_D are registered once in the rfic domain before their ODDRs to keep the
        # serializer muxes off the IOB paths at high DATA_CLK rates (491.52MHz with Oversampling).
        # Frame and data are delayed by the same cycle, so their alignment is preserved.
        tx_frame   = Signal()
        tx_frame_r = Signal()
        tx_frame_obufds = Signal()
        self.comb += [
            If(mode == AD9361PHY1R1T_MODE,
                Case(tx_count, {  # 1R1T: 4-way interleaving over 2 clocks.
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(0),
                    0b10 : tx_frame.eq(1),
                    0b11 : tx_frame.eq(0),
                })
            ).Elif(mode == AD9361PHY2R2T_MODE,
                Case(tx_count, {  # 2R2T: 8-way interleaving over 4 clocks.
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(1),
                    0b10 : tx_frame.eq(0),
                    0b11 : tx_frame.eq(0),
                })
            )
        ]
        # The data lanes are re-registered once more in their (phased) clock domains so the
        # rfic -> phased-domain crossing terminates on fabric flops instead of the IOB-locked
        # ODDR D pins; the frame gets a matching extra cycle here.
        tx_frame_r2 = Signal()
        self.sync.rfic += tx_frame_r.eq(tx_frame)
        self.sync.rfic += tx_frame_r2.eq(tx_frame_r)
        self.specials += [
            Instance("ODDR",
                p_DDR_CLK_EDGE = "SAME_EDGE",
                i_C  = ClockSignal("rfic"),
                i_CE = 1,
                i_S  = 0,
                i_R  = 0,
                i_D1 = tx_frame_r2,
                i_D2 = tx_frame_r2,
                o_Q  = tx_frame_obufds,
            ),
            Instance("OBUFDS",
                i_I  = tx_frame_obufds,
                o_O  = pads.tx_frame_p,
                o_OB = pads.tx_frame_n
            )
        ]

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
