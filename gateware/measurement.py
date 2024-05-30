#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc       import PulseSynchronizer, MultiReg
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *

# Clk Measurement ----------------------------------------------------------------------------------

class ClkMeasurement(LiteXModule):
    def __init__(self, clk, increment=1):
        self.latch = CSR()
        self.value = CSRStatus(64)

        # # #

        # Create Clock Domain.
        self.cd_counter = ClockDomain()
        self.comb += self.cd_counter.clk.eq(clk)
        self.specials += AsyncResetSynchronizer(self.cd_counter, ResetSignal())

        # Free-running Clock Counter.
        counter = Signal(64)
        self.sync.counter += counter.eq(counter + increment)

        # Latch Clock Counter.
        latch_value = Signal(64)
        latch_sync  = PulseSynchronizer("sys", "counter")
        self.submodules += latch_sync
        self.comb += latch_sync.i.eq(self.latch.re)
        self.sync.counter += If(latch_sync.o, latch_value.eq(counter))
        self.specials += MultiReg(latch_value, self.value.status)

# Multi Clk Measurement ----------------------------------------------------------------------------

class MultiClkMeasurement(LiteXModule):
    def __init__(self, clks):
        assert isinstance(clks, dict)
        for name, clk in clks.items():
            self.add_module(name=name, module=ClkMeasurement(clk))
