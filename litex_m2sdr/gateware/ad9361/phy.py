#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

# Constants ----------------------------------------------------------------------------------------

AD9361PHY2R2T_MODE = 0
AD9361PHY1R1T_MODE = 1

def phy_layout():
    layout = [
        ("ia", 12),
        ("qa", 12),
        ("ib", 12),
        ("qb", 12),
    ]
    return stream.EndpointDescription(layout)

# AD9361PHY ----------------------------------------------------------------------------------------

class AD9361PHY(LiteXModule):
    """7-Series AD9361 RFIC PHY (Dual Port Full Duplex LVDS Mode)

    This module implements a PHY for the AD9361 RFIC in Dual Port Full Duplex LVDS mode on 7-series
    FPGAs, with separate Rx_D[5:0]/Tx_D[5:0] sub-buses for simultaneous operation. It supports 1R1T
    (4-way interleaving) and 2R2T (8-way interleaving) configurations; for 2R1T/1R2T, use 2R2T mode
    with unused slots ignored.

    Samples are two's complement, MSBs first then LSBs, I/Q interleaved. D[5] is MSB, D[0] LSB per
    word.

    - In 1R1T mode: 'a' for first sample, 'b' for second; Interleave: I_MSB, Q_MSB, I_LSB,
      Q_LSB,...
    - In 2R2T mode: 'a' for channel 1, 'b' for channel 2; Interleave: I1_MSB, Q1_MSB, I1_LSB,
      Q1_LSB, I2_MSB,...

    Select mode via `mode` register; enable loopback via `loopback` register. No phase requirement
    between DATA_CLK (rx_clk) and FB_CLK (tx_clk).
    """

    def __init__(self, pads):
        self.sink    = sink   = stream.Endpoint(phy_layout())
        self.source  = source = stream.Endpoint(phy_layout())
        self.control = CSRStorage(fields=[
            CSRField("mode", size=1, offset=0, values=[
                ("``0b0``", "2R2T mode."),
                ("``0b1``", "1R1T mode."),
            ]),
            CSRField("loopback", size=1, offset=1, values=[
                ("``0b0``", "Loopback disabled."),
                ("``0b1``", "Loopback enabled."),
            ]),
        ])

        # # #

        # Signals.
        # --------
        mode     = Signal()
        loopback = Signal()
        self.specials += [
            MultiReg(self.control.fields.mode,     mode,     odomain="rfic"),
            MultiReg(self.control.fields.loopback, loopback, odomain="rfic"),
        ]

        # RX PHY -----------------------------------------------------------------------------------

        # RX Clocking.
        # ------------
        # Input: DATA_CLK from AD9361, used to clock RX data and frame.
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
        # Input: Rx_FRAME from AD9361, high for MSBs (and channel 1 in 2R2T), low for LSBs
        # (and channel 2 in 2R2T). 50% duty cycle; SDR (same value on both clock edges).
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

        # RX Count: Free-run counter; resync to 1 on Rx_FRAME rising edge.
        rx_count   = Signal(2)
        rx_frame_d = Signal()
        self.sync.rfic += [
            rx_frame_d.eq(rx_frame),
            rx_count.eq(rx_count + 1),
            If(rx_frame & ~rx_frame_d,
                Case(mode, {
                    AD9361PHY1R1T_MODE : rx_count[0].eq(1),
                    AD9361PHY2R2T_MODE : rx_count   .eq(1),
                })
            )
        ]

        # RX Data.
        # --------
        # Input: Rx_D[5:0] from AD9361, DDR: half_i captured on rising edge, half_q on falling edge.
        # Sequence: MSBs first, then LSBs; I and Q interleaved.
        rx_data_ibufds = Signal(6)
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
                    i_D  = rx_data_ibufds[i],
                    o_Q1 = rx_data_half_i[i],
                    o_Q2 = rx_data_half_q[i],
                )
            ]
        rx_data_ia  = Signal(12)
        rx_data_qa  = Signal(12)
        rx_data_ib  = Signal(12)
        rx_data_qb  = Signal(12)
        self.sync.rfic += [
            Case(rx_count[1], {
                0b0 : [ # IA/QA Shifting (assemble MSB in high bits, LSB in low).
                    rx_data_ia[0: 6].eq(rx_data_half_i),
                    rx_data_ia[6:12].eq(rx_data_ia[0:6]),
                    rx_data_qa[0: 6].eq(rx_data_half_q),
                    rx_data_qa[6:12].eq(rx_data_qa[0:6]),
                ],
                0b1 : [ # IB/QB Shifting (assemble MSB in high bits, LSB in low).
                    rx_data_ib[0: 6].eq(rx_data_half_i),
                    rx_data_ib[6:12].eq(rx_data_ib[0:6]),
                    rx_data_qb[0: 6].eq(rx_data_half_q),
                    rx_data_qb[6:12].eq(rx_data_qb[0:6]),
                ]
            })
        ]

        # RX Source Interface.
        # --------------------
        # Pulse source.valid with assembled samples.
        self.sync.rfic += [
            source.valid.eq(0),
            If(rx_count == 0,
                source.valid.eq(1),
                source.ia.eq(rx_data_ia),
                source.qa.eq(rx_data_qa),
                source.ib.eq(rx_data_ib),
                source.qb.eq(rx_data_qb),
            )
        ]

        # TX -> RX Loopback ------------------------------------------------------------------------

        self.sync.rfic += [
            If(loopback,
                source.valid.eq(sink.valid & sink.ready),
                source.ia.eq(sink.ia),
                source.qa.eq(sink.qa),
                source.ib.eq(sink.ib),
                source.qb.eq(sink.qb),
            )
        ]

        # TX PHY -----------------------------------------------------------------------------------

        # TX Sink Interface.
        # ------------------
        # Accepts new samples every 4 RFIC clocks (when tx_count == 0).

        # Control: Free-running 4-cycle counter.
        tx_count = Signal(2)
        self.sync.rfic += tx_count.eq(tx_count + 1)
        self.comb += sink.ready.eq(tx_count == 0) # Ready at 0 for sink handshake.

        # Data: Latch samples on ready; default to 0 if not valid (avoid spurs on underrun/disable).
        tx_data_ia = Signal(12)
        tx_data_qa = Signal(12)
        tx_data_ib = Signal(12)
        tx_data_qb = Signal(12)
        self.sync.rfic += [
            If(sink.ready,
                # Default to zero.
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

        # TX Clocking.
        # ------------
        # Output: FB_CLK to AD9361, derived from RFIC clock (same frequency as DATA_CLK).
        tx_clk_obufds = Signal()
        self.specials += [
            Instance("ODDR",
                p_DDR_CLK_EDGE = "SAME_EDGE",
                i_C  = ClockSignal("rfic"),
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


        # TX Framing.
        # -----------
        # Output: Tx_FRAME to AD9361, high for MSBs (and channel 1 in 2R2T), low for LSBs
        # (and channel 2 in 2R2T). 50% duty cycle; SDR (same value on both clock edges).
        tx_frame        = Signal()
        tx_frame_obufds = Signal()
        self.comb += [
            If(mode == AD9361PHY1R1T_MODE,
                Case(tx_count, { # I/Q transmitted over 2 RFIC Clk cycles (4-way interleave).
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(0),
                    0b10 : tx_frame.eq(1),
                    0b11 : tx_frame.eq(0),
                })
            ).Elif(mode == AD9361PHY2R2T_MODE,
                Case(tx_count, { # I/Q transmitted over 4 RFIC Clk cycles (8-way interleave).
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(1),
                    0b10 : tx_frame.eq(0),
                    0b11 : tx_frame.eq(0),
                })
            )
        ]
        self.specials += [
            Instance("ODDR",
                p_DDR_CLK_EDGE = "SAME_EDGE",
                i_C  = ClockSignal("rfic"),
                i_CE = 1,
                i_S  = 0,
                i_R  = 0,
                i_D1 = tx_frame,
                i_D2 = tx_frame,
                o_Q  = tx_frame_obufds,
            ),
            Instance("OBUFDS",
                i_I  = tx_frame_obufds,
                o_O  = pads.tx_frame_p,
                o_OB = pads.tx_frame_n
            )
        ]

        # TX Data.
        # --------
        # Output: Tx_D[5:0] to AD9361, DDR: half_i on rising edge, half_q on falling edge. Sequence:
        # MSBs first, then LSBs; I and Q interleaved.
        tx_data_half_i = Signal(6)
        tx_data_half_q = Signal(6)
        tx_data_obufds = Signal(6)
        self.comb += [
            Case(tx_count, {
                0b00 : [ # IA/QA MSBs.
                    tx_data_half_i.eq(tx_data_ia[6:]),
                    tx_data_half_q.eq(tx_data_qa[6:]),
                ],
                0b01 : [ # IA/QA LSBs.
                    tx_data_half_i.eq(tx_data_ia[0:]),
                    tx_data_half_q.eq(tx_data_qa[0:]),
                ],
                0b10 : [ # IB/QB MSBs.
                    tx_data_half_i.eq(tx_data_ib[6:]),
                    tx_data_half_q.eq(tx_data_qb[6:]),
                ],
                0b11 : [ # IB/QB LSBs.
                    tx_data_half_i.eq(tx_data_ib[0:]),
                    tx_data_half_q.eq(tx_data_qb[0:]),
                ]
            })
        ]
        for i in range(6):
            self.specials += [
                Instance("ODDR",
                    p_DDR_CLK_EDGE = "SAME_EDGE",
                    i_C  = ClockSignal("rfic"),
                    i_CE = 1,
                    i_S  = 0,
                    i_R  = 0,
                    i_D1 = tx_data_half_i[i],
                    i_D2 = tx_data_half_q[i],
                    o_Q  = tx_data_obufds[i],
                ),
                Instance("OBUFDS",
                    i_I  = tx_data_obufds[i],
                    o_O  = pads.tx_data_p[i],
                    o_OB = pads.tx_data_n[i]
                )
            ]
