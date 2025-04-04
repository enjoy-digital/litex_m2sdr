#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litepcie.common import *

# RX GPIO Packer -----------------------------------------------------------------------------------

class RXGPIOPacker(LiteXModule):
    """Packs 4 GPIO inputs into the 4 spare bits of a 16-bit RX I/Q stream."""
    def __init__(self):
        self.gpio_i = Signal(4)                       # GPIO Inputs.
        self.sink   = stream.Endpoint(dma_layout(64)) # Input  I/Q stream (12-bit data).
        self.source = stream.Endpoint(dma_layout(64)) # Output I/Q stream (12-bit + 4-bit GPIO).

        # # #

        # TODO.

# TX GPIO Unpacker ---------------------------------------------------------------------------------

class TXGPIOUnpacker(LiteXModule):
    """Unpacks 4 GPIO outputs and tristate controls from a 16-bit TX I/Q stream."""
    def __init__(self):
        self.gpio_oe = Signal(4)                       # GPIO Output Enable.
        self.gpio_o  = Signal(4)                       # GPIO Output.
        self.sink    = stream.Endpoint(dma_layout(64)) # Input  I/Q stream (12-bit + 4-bit GPIO).
        self.source  = stream.Endpoint(dma_layout(64)) # Output I/Q stream (12-bit data).

        # # #

        # TODO.

# GPIO ---------------------------------------------------------------------------------------------

class GPIO(LiteXModule):
    """
    GPIO Module for LiteX M2SDR with CSR control and Packer/Unpacker integration.

    Features:
    - 4-bit GPIO with synchronous RX sampling and TX control.
    - Switch between Packer/Unpacker or CSR control.
    - Tristate control via CSR.
    """
    def __init__(self):
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
            # GPIO Outputs.
            # -------------

            # Default to tristate, overridden by enable and mode
            self.o.eq(0),  # Default: All outputs low.
            self.oe.eq(0), # Default: All outputs tristated.

            If(self._control.fields.enable,
                # Packer/Unpacker mode.
                If(self._control.fields.source == 0,
                    self.o.eq(0),  # FIXME: From TXGPIOUnpacker.
                    self.oe.eq(0), # FIXME: From TXGPIOUnpacker.
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
            # FIXME: Connect self.rx_packer.gpio.eq(self.i) for RX sampling
        ]
