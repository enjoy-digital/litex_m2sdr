#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *

# Time Generator -----------------------------------------------------------------------------------

class TimeGenerator(LiteXModule):
    def __init__(self, clk, clk_freq, init=0, with_csr=True):
        assert 1e9/clk_freq == int(1e9/clk_freq)
        self.enable          = Signal()
        self.write           = Signal()
        self.write_time      = Signal(64)
        self.time            = Signal(64)
        self.time_adjustment = Signal(64)
        self.time_change     = Signal()

        # # #

        # Time Signals.
        time = Signal(64, reset=init)

        # Time Clk Domain.
        self.cd_time = ClockDomain()
        self.comb += self.cd_time.clk.eq(clk)

        # Time Handling.
        self.sync.time += [
            # Disable: Reset Time to 0.
            If(~self.enable,
                time.eq(init),
            # Software Write.
            ).Elif(self.write,
                time.eq(self.write_time),
            # Increment.
            ).Else(
                time.eq(time + int(1e9/clk_freq)),
            ),
            self.time.eq(time + self.time_adjustment)
        ]

        # CSRs.
        if with_csr:
            self.add_csr()

    def add_csr(self, default_enable=1):
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "Time Generator Disabled."),
                ("``0b1``", "Time Generator Enabled."),
            ], reset=default_enable),
            CSRField("read",  size=1, offset=1, pulse=True),
            CSRField("write", size=1, offset=2, pulse=True),
        ])
        self._read_time       = CSRStatus(64,  description="Read Time  (ns) (FPGA Time -> SW).")
        self._write_time      = CSRStorage(64, description="Write Time (ns) (SW Time -> FPGA).")
        self._time_adjustment = CSRStorage(64, description="Time Adjustment Value (ns) (SW -> FPGA).")

        # # #

        # Enable.
        self.specials += MultiReg(self._control.fields.enable, self.enable)

        # Time Read (FPGA -> SW).
        time_read = Signal(64)
        time_read_ps = PulseSynchronizer("sys", "time")
        self.submodules += time_read_ps
        self.comb += time_read_ps.i.eq(self._control.fields.read)
        self.sync.time += If(time_read_ps.o, time_read.eq(self.time))
        self.specials += MultiReg(time_read, self._read_time.status)

        # Time Write (SW -> FPGA).
        self.specials += MultiReg(self._write_time.storage, self.write_time, "time")
        time_write_ps = PulseSynchronizer("sys", "time")
        self.submodules += time_write_ps
        self.comb += time_write_ps.i.eq(self._control.fields.write)
        self.comb += self.write.eq(time_write_ps.o)

        # Time Adjust (SW -> FPGA).
        self.specials += MultiReg(self._time_adjustment.storage, self.time_adjustment, "time")

        # Time Change.
        time_change_ps = PulseSynchronizer("sys", "time")
        self.submodules += time_change_ps
        self.comb += time_change_ps.i.eq(self._control.fields.write | self._time_adjustment.re)
        self.comb += self.time_change.eq(time_change_ps.o)
