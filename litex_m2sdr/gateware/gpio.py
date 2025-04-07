#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.build.io import SDRTristate

from litex.gen import *

from litepcie.common import *

# GPIO RX Packer -----------------------------------------------------------------------------------

class GPIORXPacker(LiteXModule):
    """
    Packs 4 GPIO inputs into the 4 spare bits of a 16-bit RX I/Q stream.

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
        |0000DDDDDDD|GGGGDDDDDDD|0000DDDDDDD|GGGGDDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - GGGG = 4-bit GPIO data inserted into IA[15:12] and IB[15:12].
        - QA[15:12] and QB[15:12] are zeroed.
        - DDDDDDD (bits [11:0]) remain unchanged from the original I/Q data.
    """
    def __init__(self):
        self.enable  = Signal()                                 # GPIO Module Enable.
        self.i       = Signal(4)                                # GPIO Inputs.
        self.sink    = sink   = stream.Endpoint(dma_layout(64)) # Input  I/Q stream (12-bit data).
        self.source  = source = stream.Endpoint(dma_layout(64)) # Output I/Q stream (12-bit + 4-bit GPIO).

        # # #

        # Connect Sink to Source.
        self.comb += sink.connect(source)

        # Input Mapping.
        enable = Signal()
        self.specials += MultiReg(self.enable, enable, "rfic")
        self.comb += If(enable,
            source.data[0*16+12:1*16].eq(self.i),  # Pack GPIO into IA[15:12].
            source.data[1*16+12:2*16].eq(0),       # QA[15:12] zeroed.
            source.data[2*16+12:3*16].eq(self.i),  # Pack GPIO into IB[15:12].
            source.data[3*16+12:4*16].eq(0),       # QB[15:12] zeroed.
        )

# GPIO TX Unpacker ---------------------------------------------------------------------------------

class GPIOTXUnpacker(LiteXModule):
    """
    Unpacks 4 GPIO outputs and tristate controls from a 16-bit TX I/Q stream.

    Data Format (64-bit stream):
        Input (sink.data from Pipeline):
        +------------------------------------------------------------------+
        | 63      48|47       32|31       16|15        0| <- Bit Positions |
        | QB        | IB        | QA        | IA        | <- 16-bit Fields |
        |[15..0]    |[15..0]    |[15..0]    |[15..0]    |                  |
        |xxxxDDDDDDD|xxxxDDDDDDD|TTTTDDDDDDD|OOOODDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - OOOO = 4-bit GPIO Outputs in IA[15:12].
        - TTTT = 4-bit Tristate Controls in QA[15:12].
        - xxxx = unused/don't care bits [15:12] in IB and QB.
        - DDDDDDD = 12-bit I/Q data in [11:0] of all fields.

        Output (source.data to RFIC):
        +------------------------------------------------------------------+
        | 63      48|47       32|31       16|15        0| <- Bit Positions |
        | QB        | IB        | QA        | IA        | <- 16-bit Fields |
        |[15..0]    |[15..0]    |[15..0]    |[15..0]    |                  |
        |xxxxDDDDDDD|xxxxDDDDDDD|TTTTDDDDDDD|OOOODDDDDDD| <- Layout        |
        +------------------------------------------------------------------+
        - Identical to input; full 16-bit words passed to PHY unchanged.
        - GPIO Outputs (o) extracted from IA[15:12].
        - Tristate Controls (oe) extracted from QA[15:12].
    """
    def __init__(self):
        self.enable = Signal()                                # GPIO Module Enable.
        self.oe     = Signal(4)                               # GPIO Output Enable.
        self.o      = Signal(4)                               # GPIO Output.
        self.sink   = sink   = stream.Endpoint(dma_layout(64)) # Input  I/Q stream (12-bit + 4-bit GPIO).
        self.source = source = stream.Endpoint(dma_layout(64)) # Output I/Q stream (12-bit data).

        # # #

        # Connect Sink to Source.
        self.comb += sink.connect(source)

        # Output Mapping.
        enable = Signal()
        self.specials += MultiReg(self.enable, enable, "rfic")
        self.comb += If(enable,
            self.o.eq(source.data[0*16+12:1*16]),  # Unpack GPIO outputs from IA[15:12].
            self.oe.eq(source.data[1*16+12:2*16]), # Unpack tristate controls from QA[15:12].
        )

# GPIO ---------------------------------------------------------------------------------------------

class GPIO(LiteXModule):
    """
    GPIO Module for LiteX M2SDR with CSR control and Packer/Unpacker integration.

    Features:
    - 4-bit GPIO with synchronous RX sampling and TX control.
    - Switch between Packer/Unpacker or CSR control.
    - Tristate control via CSR.
    """
    def __init__(self, rx_packer, tx_unpacker):
         # IO Signals
        self.o  = Signal(4)  # Output data.
        self.oe = Signal(4)  # Output enable (1=drive, 0=tristate).
        self.i  = Signal(4)  # Input data.

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
            self.o.eq(0),  # Default: All outputs low.
            self.oe.eq(0), # Default: All outputs tristated.

            If(self._control.fields.enable,
                # Packer/Unpacker mode.
                If(self._control.fields.source == 0,
                    self.o.eq(tx_unpacker.o),
                    self.oe.eq(tx_unpacker.oe),
                ),
                # CSR mode.
                If(self._control.fields.source == 1,
                    self.o.eq(self._o.fields.data),
                    self.oe.eq(self._oe.fields.enable),
                ),
            ),

            # GPIO Inputs.
            # ------------
            self._i.fields.data.eq(self.i),
            rx_packer.i.eq(self.i),
        ]

    def connect_to_pads(self, pads):
        for i in range(len(pads)):
            self.specials += SDRTristate(pads[i],
                o   = self.o[i],
                oe  = self.oe[i],
                i   = self.i[i],
                clk = ClockSignal("rfic")
            )
