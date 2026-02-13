#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2025-2026 Enjoy-Digital <enjoy-digital.fr>
# Sponsored by tii.ae.
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.build.io import DDRTristate

from litex.gen import *

from litepcie.common import *

# GPIO RX Packer -----------------------------------------------------------------------------------

class GPIORXPacker(LiteXModule):
    """
    Packs 4 GPIO inputs into the 4 spare bits of a 16-bit RX I/Q stream using DDR.

    Data Format (64-bit stream):
        Before Packing (sink.data from RFIC):
        +------------------------------------------------------------------+
        | 63      48|47       32|31       16|15        0| <- Bit Positions |
        | QB        | IB        | QA        | IA        | <- 16-bit Fields |
        |[15..0]    |[15..0]    |[15..0]    |[15..0]    |                  |
        |SSSSDDDDDDD|SSSSDDDDDDD|SSSSDDDDDDD|SSSSDDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - Each 16-bit field (IA, QA, IB, QB) contains a 12-bit I/Q sample.
        - S = sign-extended bits [15:12], D = data bits [11:0].

        After Packing (source.data to Pipeline):
        +------------------------------------------------------------------+
        | 63      48|47       32|31       16|15        0| <- Bit Positions |
        | QB        | IB        | QA        | IA        | <- 16-bit Fields |
        |[15..0]    |[15..0]    |[15..0]    |[15..0]    |                  |
        |0000DDDDDDD|iiiiDDDDDDD|0000DDDDDDD|IIIIDDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - IIII = 4-bit GPIO data (i1) inserted into IA[15:12] (first clock edge).
        - iiii = 4-bit GPIO data (i2) inserted into IB[15:12] (second clock edge).
        - QA[15:12] and QB[15:12] are zeroed.
        - DDDDDDD (bits [11:0]) remain unchanged from the original I/Q data.
    """
    def __init__(self):
        self.enable  = Signal()                                 # GPIO Module Enable.
        self.i1      = Signal(4)                                # GPIO Inputs (first edge).
        self.i2      = Signal(4)                                # GPIO Inputs (second edge).
        self.sink    = sink   = stream.Endpoint(dma_layout(64)) # Input  I/Q stream (12-bit data).
        self.source  = source = stream.Endpoint(dma_layout(64)) # Output I/Q stream (12-bit + 4-bit GPIO).

        # # #

        # Connect Sink to Source.
        self.comb += sink.connect(source)

        # Input Mapping.
        enable = Signal()
        self.specials += MultiReg(self.enable, enable, "rfic")
        self.comb += If(enable,
            source.data[0*16+12:1*16].eq(self.i1),  # Pack GPIO i1 into IA[15:12].
            source.data[1*16+12:2*16].eq(0),        # QA[15:12] zeroed.
            source.data[2*16+12:3*16].eq(self.i2),  # Pack GPIO i2 into IB[15:12].
            source.data[3*16+12:4*16].eq(0),        # QB[15:12] zeroed.
        )

# GPIO TX Unpacker ---------------------------------------------------------------------------------

class GPIOTXUnpacker(LiteXModule):
    """
    Unpacks 4 GPIO outputs and tristate controls from a 16-bit TX I/Q stream using DDR.

    Data Format (64-bit stream):
        Input (sink.data from Pipeline):
        +------------------------------------------------------------------+
        | 63      48|47       32|31       16|15        0| <- Bit Positions |
        | QB        | IB        | QA        | IA        | <- 16-bit Fields |
        |[15..0]    |[15..0]    |[15..0]    |[15..0]    |                  |
        |TTTTDDDDDDD|OOOODDDDDDD|ttttDDDDDDD|ooooDDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - oooo = 4-bit GPIO Outputs (o1) in IA[15:12] (first edge).
        - tttt = 4-bit Tristate Controls (oe1) in QA[15:12] (first edge).
        - OOOO = 4-bit GPIO Outputs (o2) in IB[15:12] (second edge).
        - TTTT = 4-bit Tristate Controls (oe2) in QB[15:12] (second edge).
        - DDDDDDD = 12-bit I/Q data in [11:0] of all fields.

        Output (source.data to RFIC):
        +------------------------------------------------------------------+
        | 63      48|47       32|31       16|15        0| <- Bit Positions |
        | QB        | IB        | QA        | IA        | <- 16-bit Fields |
        |[15..0]    |[15..0]    |[15..0]    |[15..0]    |                  |
        |TTTTDDDDDDD|OOOODDDDDDD|ttttDDDDDDD|ooooDDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - Identical to input; full 16-bit words passed to PHY unchanged.
        - o1, oe1 extracted from IA[15:12], QA[15:12] (first edge).
        - o2, oe2 extracted from IB[15:12], QB[15:12] (second edge).
    """
    def __init__(self):
        self.enable = Signal()                                # GPIO Module Enable.
        self.o1     = Signal(4)                               # GPIO Outputs (first edge).
        self.oe1    = Signal(4)                               # Tristate Controls (first edge).
        self.o2     = Signal(4)                               # GPIO Outputs (second edge).
        self.oe2    = Signal(4)                               # Tristate Controls (second edge).
        self.sink   = sink   = stream.Endpoint(dma_layout(64)) # Input  I/Q stream (12-bit + 4-bit GPIO).
        self.source = source = stream.Endpoint(dma_layout(64)) # Output I/Q stream (12-bit data).

        # # #

        # Connect Sink to Source.
        self.comb += sink.connect(source)

        # Output Mapping.
        enable = Signal()
        self.specials += MultiReg(self.enable, enable, "rfic")
        self.comb += If(enable,
            self.o1.eq( source.data[0*16+12:1*16]), # Unpack GPIO o1 from IA[15:12].
            self.oe1.eq(source.data[1*16+12:2*16]), # Unpack tristate oe1 from QA[15:12].
            self.o2.eq( source.data[2*16+12:3*16]), # Unpack GPIO o2 from IB[15:12].
            self.oe2.eq(source.data[3*16+12:4*16]), # Unpack tristate oe2 from QB[15:12].
        )

# GPIO ---------------------------------------------------------------------------------------------

class GPIO(LiteXModule):
    """
    GPIO Module for LiteX M2SDR with CSR control and Packer/Unpacker integration using DDR.

    Features:
    - 4-bit GPIO with DDR synchronous RX sampling and TX driving.
    - Switch between Packer/Unpacker or CSR control with optional loopback mode.
    - Tristate control via CSR.
    - Loopback mode connects o1 to i1 and o2 to i2 internally, bypassing DDRTristate.
    """
    def __init__(self, rx_packer, tx_unpacker):
        # IO Signals
        self.o1  = Signal(4)  # Output data (first edge).
        self.o2  = Signal(4)  # Output data (second edge).
        self.oe1 = Signal(4)  # Output enable (first edge,  1=drive, 0=tristate).
        self.oe2 = Signal(4)  # Output enable (second edge, 1=drive, 0=tristate).
        self.i1  = Signal(4)  # Input data (first edge).
        self.i2  = Signal(4)  # Input data (second edge).

        # # #

        # GPIO Control/Status Registers ------------------------------------------------------------

        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "GPIO module disabled."),
                ("``0b1``", "GPIO module enabled."),
            ], description="Enable/disable GPIO functionality."),
            CSRField("source", size=1, offset=1, values=[
                ("``0b0``", "GPIO controlled by Packer/Unpacker."),
                ("``0b1``", "GPIO controlled by CSR."),
            ], description="Select GPIO control source."),
            CSRField("loopback", size=1, offset=2, values=[
                ("``0b0``", "Normal operation (DDRTristate active)."),
                ("``0b1``", "Loopback mode (o1 to i1, o2 to i2)."),
            ], description="Enable/disable internal loopback mode."),
        ])

        self._o = CSRStorage(fields=[
            CSRField("data", size=4, offset=0, values=[
                ("``0b0000``", "All GPIO outputs low."),
                ("``0b1111``", "All GPIO outputs high."),
            ], description="GPIO output data when in CSR mode."),
        ])

        self._oe = CSRStorage(fields=[
            CSRField("enable", size=4, offset=0, values=[
                ("``0b0000``", "All GPIO pins tristated."),
                ("``0b1111``", "All GPIO pins driven."),
            ], description="Output enable (tristate control) when in CSR mode."),
        ])

        self._i = CSRStatus(fields=[
            CSRField("data", size=4, offset=0, values=[
                ("``0b0000``", "All GPIO inputs low."),
                ("``0b1111``", "All GPIO inputs high."),
            ], description="GPIO input data."),
        ])

        # GPIO Control Logic -----------------------------------------------------------------------

        self.comb += [
            # GPIO Enable.
            # ------------
            If(self._control.fields.enable,
                rx_packer.enable.eq(1),
                tx_unpacker.enable.eq(1),
            ),

            # GPIO Outputs.
            # -------------

            # Default to tristate, overridden by enable and mode
            self.o1.eq(0),  # Default: All outputs low (first edge).
            self.o2.eq(0),  # Default: All outputs low (second edge).
            self.oe1.eq(0), # Default: All outputs tristated (first edge).
            self.oe2.eq(0), # Default: All outputs tristated (second edge).

            # Packer/Unpacker mode.
            If(self._control.fields.source == 0,
                self.o1.eq( tx_unpacker.o1),
                self.oe1.eq(tx_unpacker.oe1),
                self.o2.eq( tx_unpacker.o2),
                self.oe2.eq(tx_unpacker.oe2),
            ),
            # CSR mode (duplicate data across edges).
            If(self._control.fields.source == 1,
                self.o1.eq( self._o.fields.data),
                self.o2.eq( self._o.fields.data),
                self.oe1.eq(self._oe.fields.enable),
                self.oe2.eq(self._oe.fields.enable),
            ),

            # GPIO Inputs.
            # ------------
            If(self._control.fields.loopback,
                self._i.fields.data.eq(self.o1),
                rx_packer.i1.eq(self.o1),  # Loop o1  (first edge) to i1.
                rx_packer.i2.eq(self.o2),  # Loop o2 (second edge) to i2.
            ).Else(
                self._i.fields.data.eq(self.i1),
                rx_packer.i1.eq(self.i1),
                rx_packer.i2.eq(self.i2),
            ),
        ]

    def connect_to_pads(self, pads):
        self.i_async = Signal(len(pads))
        for i in range(len(pads)):
            self.specials += DDRTristate(pads[i],
                # Clk.
                clk = ClockSignal("rfic"),

                # Output.
                o1  = self.o1[i],
                o2  = self.o2[i],
                oe1 = self.oe1[i],
                oe2 = self.oe2[i],

                # Input.
                i1  = self.i1[i],
                i2  = self.i2[i],

                # Input (Async).
                i_async = self.i_async[i],
            )
