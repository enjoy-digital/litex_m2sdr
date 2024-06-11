#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litepcie.common import *

from litex.soc.interconnect.csr import *

# Timestamp ----------------------------------------------------------------------------------------

class Timestamp(LiteXModule):
    def __init__(self, clk_domain, with_csr=True):
        self.time = time = Signal(64) # o
        self.pps  = pps  = Signal()   # i

        self.enable    = Signal()   # i
        self.set       = Signal()   # i
        self.set_time  = Signal(64) # i

        # # #

        # Time Clk Domain.
        self.cd_time = ClockDomain()
        self.comb += [
            self.cd_time.clk.eq(ClockSignal(clk_domain)),
            self.cd_time.rst.eq(ResetSignal(clk_domain)),
        ]

        # Internal 32-bit counters.
        time_low  = Signal(32)
        time_high = Signal(32)

        # Time Handling.
        self.sync.time += [
            # Disable: Reset Time to 0.
            If(~self.enable,
                time_low.eq(0),
                time_high.eq(0),
            # Increment.
            ).Else(
                If(time_low == 0xffffffff,
                    time_low.eq(0),
                    time_high.eq(time_high + 1)
                ).Else(
                    time_low.eq(time_low + 1)
                )
            )
        ]

        # Combine low and high counters into 64-bit time signal.
        self.comb += time.eq(Cat(time_low, time_high))

        # PPS Resync/Edge.
        _pps      = Signal()
        _pps_d    = Signal()
        _pps_edge = Signal()
        self.specials += MultiReg(pps, _pps)
        self.sync.time += _pps_d.eq(_pps)
        self.comb += _pps_edge.eq(_pps & ~_pps_d)

        # Time Set.
        _set = Signal()
        self.sync.time += [
            # Disable: Reset to 0.
            If(~self.enable,
                _set.eq(0),
            ),
            # Set time and set _set until next PPS edge.
            If(self.set | _set,
                _set.eq(1),
                time_low.eq(self.set_time[:32]),
                time_high.eq(self.set_time[32:]),
                If(_pps_edge,
                    _set.eq(0)
                ),
            ),
        ]

        # CSRs.
        if with_csr:
            self.add_csr(clk_domain)

    def add_csr(self, clk_domain, default_enable=1):
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "Timestamp Module Disabled."),
                ("``0b1``", "Timestamp Module Enabled."),
            ], reset=default_enable),
            CSRField("latch", size=1, offset=1, pulse=True, description="Latch Timestamp."),
            CSRField("set",   size=1, offset=2, pulse=True, description="Set Timestamp on next PPS edge."),
        ])
        self._latch_time = CSRStatus(64,  description="Timestamp of last Time latch (in JESD Clk Cycles).")
        self._set_time   = CSRStorage(64, description="Timestamp to set on next PPS edge (in JESD Clk Cycles).")

        # # #

        # Timestamp Enable.
        self.specials += MultiReg(self._control.fields.enable, self.enable)

        # Timestamp Latch.
        time_latch    = Signal(64)
        time_latch_ps = PulseSynchronizer("sys", "time")
        self.submodules += time_latch_ps
        self.comb += time_latch_ps.i.eq(self._control.fields.latch)
        self.sync.time += If(time_latch_ps.o,
            time_latch.eq(self.time),
        )
        self.specials += MultiReg(time_latch, self._latch_time.status)

        # Timestamp Set.
        self.specials += MultiReg(self._set_time.storage, self.set_time, "time")
        time_set_ps = PulseSynchronizer("sys", "time")
        self.submodules += time_set_ps
        self.comb += time_set_ps.i.eq(self._control.fields.set)
        self.comb += self.set.eq(time_set_ps.o)
