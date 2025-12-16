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
    """
    def __init__(self, pads):
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

        # # #

        # Control Signals
        # ---------------
        mode     = Signal()  # Mode selection (1R1T or 2R2T).
        loopback = Signal()  # Loopback enable/disable.
        self.specials += [
            MultiReg(self.control.fields.mode,     mode,     odomain="rfic"),
            MultiReg(self.control.fields.loopback, loopback, odomain="rfic"),
        ]

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
            Instance("IBUFDS", # IBUFDS for differential to single-ended conversion (for robustness) 
                i_I  = pads.rx_frame_p, # signal from AD9361 indicating whether the RX frame is valid
                i_IB = pads.rx_frame_n,
                o_O  = rx_frame_ibufds # RX frame is valid
            ),
            Instance("IDDR",
                p_DDR_CLK_EDGE = "SAME_EDGE_PIPELINED",
                i_C  = ClockSignal("rfic"),
                i_CE = 1,
                i_S  = 0,
                i_R  = 0,
                i_D  = rx_frame_ibufds,
                o_Q1 = rx_frame, # rx_frame is used for sample selection (2R2T) or channel selection (1R1T). it comes from 
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
                    o_Q1 = rx_data_half_i[i], # I on rising edge
                    o_Q2 = rx_data_half_q[i], # Q on falling edge
                )
            ]
        rx_data_ia  = Signal(12)
        rx_data_qa  = Signal(12)
        rx_data_ib  = Signal(12)
        rx_data_qb  = Signal(12)
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
        # Outputs assembled RX samples when valid (rx_count == 0).
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

        # Routes TX data to RX when loopback is enabled.
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
        # Accepts new TX samples every 4 RFIC clock cycles (tx_count == 0).
        tx_count = Signal(2)
        self.sync.rfic += tx_count.eq(tx_count + 1)
        self.comb += sink.ready.eq(tx_count == 0)  # Ready for new data at cycle 0.

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


        # TX Framing
        # ----------
        # Generates TX_FRAME: high for MSBs (or channel 1 in 2R2T), low for LSBs(or channel 2 in
        # 2R2T). Uses SDR with 50% duty cycle.
        tx_frame = Signal()
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

        # TX Data
        # -------
        # Outputs TX_D[5:0] in DDR mode: I samples on rising edge, Q samples on falling edge.
        # Sequences MSBs first, then LSBs; I and Q interleaved. Relies on AD9361's alignment for
        # correct data transmission.
        tx_data_half_i = Signal(6)
        tx_data_half_q = Signal(6)
        tx_data_obufds = Signal(6)
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
