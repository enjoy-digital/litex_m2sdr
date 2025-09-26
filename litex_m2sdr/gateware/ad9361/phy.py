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
    """7-Series AD9361 RFIC PHY

    This module implements a PHY for the AD9361 RFIC in LVDS mode on 7-series FPGAs. It supports
    both 1R1T and 2R2T configurations.

    - In 1R1T mode, the 'a' suffix is used for the first sample, and 'b' for the second sample.
    - In 2R2T mode, the 'a' suffix is used for channel 1 samples, and 'b' for channel 2 samples.

    The operating mode can be selected through the `mode` register. Additionally, a dynamic loopback
    feature is supported, which can be enabled or disabled through the `loopback` register.
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
        rx_frame_ibufds   = Signal()
        rx_frame          = Signal()
        rx_frame_d        = Signal()
        rx_frame_rising   = Signal()
        rx_frame_rising_d = Signal()
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
        self.sync.rfic += rx_frame_d.eq(rx_frame)
        self.comb += rx_frame_rising.eq(rx_frame & ~rx_frame_d)
        self.sync.rfic += rx_frame_rising_d.eq(rx_frame_rising)

        # RX Data.
        # --------
        # I sampled on rfic clk  rising edge.
        # Q sampled on rfic clk falling edge.
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

        # rx_frame = 1 / IA/QA.
        # rx_frame = 0 / IB/QB.
        rx_frame_first = Signal()
        rx_data_valid  = Signal(4)
        rx_data_ia     = Signal(12)
        rx_data_qa     = Signal(12)
        rx_data_ib     = Signal(12)
        rx_data_qb     = Signal(12)
        self.sync.rfic += [
            If(mode == AD9361PHY1R1T_MODE,
                rx_data_valid.eq(Cat(rx_frame_rising & rx_frame_first, rx_data_valid)),
                If(rx_frame_rising_d, rx_frame_first.eq(~rx_frame_first))
            ).Elif(mode == AD9361PHY2R2T_MODE,
                rx_data_valid.eq(Cat(rx_frame_rising, rx_data_valid))
            )
        ]
        self.sync.rfic += [
            If(mode == AD9361PHY1R1T_MODE,
                If(rx_frame_first,
                    rx_data_ia.eq(Cat(rx_data_half_i, rx_data_ia)),
                    rx_data_qa.eq(Cat(rx_data_half_q, rx_data_qa)),
                ).Else(
                    rx_data_ib.eq(Cat(rx_data_half_i, rx_data_ib)),
                    rx_data_qb.eq(Cat(rx_data_half_q, rx_data_qb)),
                )
            ).Elif(mode == AD9361PHY2R2T_MODE,
                If(rx_frame,
                    rx_data_ia.eq(Cat(rx_data_half_i, rx_data_ia)),
                    rx_data_qa.eq(Cat(rx_data_half_q, rx_data_qa)),
                ).Else(
                    rx_data_ib.eq(Cat(rx_data_half_i, rx_data_ib)),
                    rx_data_qb.eq(Cat(rx_data_half_q, rx_data_qb)),
                )
            )
        ]

        # Drive Source.
        self.sync.rfic += [
            source.valid.eq(0),
            If(rx_data_valid[3],
                source.valid.eq(1),
                source.ia.eq(rx_data_ia),
                source.qa.eq(rx_data_qa),
                source.ib.eq(rx_data_ib),
                source.qb.eq(rx_data_qb)
            )
        ]

        # TX PHY -----------------------------------------------------------------------------------

        # TX Clocking.
        # ------------
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

        # TX Gen from Sink.
        # -----------------
        # TX always supposed valid.
        tx_ce  = Signal()
        tx_cnt = Signal(2)
        self.sync.rfic += tx_cnt.eq(tx_cnt + 1)
        self.comb += tx_ce.eq(tx_cnt == 3)

        tx_data_ia    = Signal(12)
        tx_data_qa    = Signal(12)
        tx_data_ib    = Signal(12)
        tx_data_qb    = Signal(12)
        self.sync.rfic += [
            If(tx_ce,
                tx_data_ia.eq(0),
                tx_data_qa.eq(0),
                tx_data_ib.eq(0),
                tx_data_qb.eq(0),
                If(sink.valid,
                    tx_data_ia.eq(sink.ia),
                    tx_data_qa.eq(sink.qa),
                    tx_data_ib.eq(sink.ib),
                    tx_data_qb.eq(sink.qb),
                )
            )
        ]
        self.comb += sink.ready.eq(tx_ce)

        # TX -> RX Dynamic Loopback.
        # --------------------------
        self.sync.rfic += [
            If(loopback,
                source.valid.eq(sink.valid & sink.ready),
                source.ia.eq(sink.ia),
                source.qa.eq(sink.qa),
                source.ib.eq(sink.ib),
                source.qb.eq(sink.qb),
            )
        ]

        # TX Framing.
        # -----------
        tx_frame       = Signal()
        tx_data_half_i = Signal(6)
        tx_data_half_q = Signal(6)
        self.comb += [
            Case(tx_cnt, {
                0b00 : [
                    tx_data_half_i.eq(tx_data_ia[6:]),
                    tx_data_half_q.eq(tx_data_qa[6:]),
                ],
                0b01 : [
                    tx_data_half_i.eq(tx_data_ia[0:]),
                    tx_data_half_q.eq(tx_data_qa[0:]),
                ],
                0b10 : [
                    tx_data_half_i.eq(tx_data_ib[6:]),
                    tx_data_half_q.eq(tx_data_qb[6:]),
                ],
                0b11 : [
                    tx_data_half_i.eq(tx_data_ib[0:]),
                    tx_data_half_q.eq(tx_data_qb[0:]),
                ]
            }),
            If(mode == AD9361PHY1R1T_MODE,
                Case(tx_cnt, {
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(0),
                    0b10 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(0),
                })
            ).Elif(mode == AD9361PHY2R2T_MODE,
                Case(tx_cnt, {
                    0b00 : tx_frame.eq(1),
                    0b01 : tx_frame.eq(1),
                    0b10 : tx_frame.eq(0),
                    0b01 : tx_frame.eq(0),
                })
            )
        ]

        tx_frame_obufds = Signal()
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
        tx_data_obufds = Signal(6)
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
