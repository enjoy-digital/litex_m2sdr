#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc       import PulseSynchronizer, MultiReg
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *

# Clk Measurement ----------------------------------------------------------------------------------

class ClkMeasurement(LiteXModule):
    def __init__(self, clk, increment=1, with_csr=True):
        self.latch = Signal()
        self.value = Signal(64)

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
        self.comb += latch_sync.i.eq(self.latch)
        self.sync.counter += If(latch_sync.o, latch_value.eq(counter))
        self.specials += MultiReg(latch_value, self.value)

        # CSR (Optional).
        if with_csr:
            self.add_csr()

    def add_csr(self):
        self._latch = CSR()
        self._value = CSRStatus(64)
        self.comb += [
            If(self._latch.re, self.latch.eq(1)),
            self._value.status.eq(self.value),
        ]

# Multi Clk Measurement ----------------------------------------------------------------------------

class MultiClkMeasurement(LiteXModule):
    def __init__(self, clks, with_csr=True, with_latch_all=True):
        assert isinstance(clks, dict)

        # Clock Measurement Modules.
        self.clk_modules = {}
        for name, clk in clks.items():
            self.clk_modules[name] = ClkMeasurement(clk, with_csr=with_csr)
            self.add_module(name=name, module=self.clk_modules[name])

        # Latch All CSR (Optional)
        if with_csr and with_latch_all:
            self.latch_all = CSR()
            for name, mod in self.clk_modules.items():
                self.comb += If(self.latch_all.re, mod.latch.eq(1))
