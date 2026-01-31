#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect import stream

# Time Generator -----------------------------------------------------------------------------------

class TimeGenerator(LiteXModule):
    def __init__(self, clk, clk_freq, init=0, with_csr=True):
        time_inc_reset = int(round((1e9/clk_freq) * (1 << 24)))
        assert time_inc_reset < (1 << 32)
        self.enable          = Signal()
        self.sync_enable     = Signal()
        self.write           = Signal()
        self.write_time      = Signal(64)
        self.time            = Signal(64)
        self.time_adjustment = Signal(64)
        self.time_change     = Signal()
        # Time increment in ns per tick (Q8.24 fixed-point). Allows fractional ns.
        self.time_inc        = Signal(32, reset=time_inc_reset)

        # Time Sync Interface.
        self.time_sync    = Signal()
        self.time_seconds = Signal(40)

        # # #

        # Time Signals.
        time = Signal(64, reset=init)
        frac = Signal(24)

        # Time Clk Domain.
        self.cd_time = ClockDomain()
        self.comb += self.cd_time.clk.eq(clk)

        # Time Handling (Q8.24 fractional ns).
        self.sync.time += [
            # Disable: Reset Time to 0.
            If(~self.enable,
                time.eq(init),
                frac.eq(0),
            # Software Write.
            ).Elif(self.write,
                time.eq(self.write_time),
                frac.eq(0),
            # Time Sync.
            ).Elif(self.sync_enable & self.time_sync,
                time.eq(((self.time_seconds + 1)* int(1e9))),
                frac.eq(0),
            # Increment (fractional).
            ).Else(
                Cat(frac, time).eq(Cat(frac, time) + self.time_inc),
            ),
            self.time.eq(time + self.time_adjustment)
        ]

        # CSRs.
        if with_csr:
            self.add_csr()

    def add_csr(self, default_enable=1, default_pps_align=0):
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "Time Generator Disabled."),
                ("``0b1``", "Time Generator Enabled."),
            ], reset=default_enable),
            CSRField("read",  size=1, offset=1, pulse=True),
            CSRField("write", size=1, offset=2, pulse=True),
            CSRField("sync_enable", size=1, offset=3, values=[
                ("``0b0``", "Sync (Re-)Alignment Disabled."),
                ("``0b1``", "Sync (Re-)Alignment Enabled."),
            ], reset=default_pps_align),
        ])
        self._read_time       = CSRStatus(64,  description="Read Time  (ns) (FPGA Time -> SW).")
        self._write_time      = CSRStorage(64, description="Write Time (ns) (SW Time -> FPGA).")
        self._time_adjustment = CSRStorage(64, description="Time Adjustment Value (ns) (SW -> FPGA).")
        self._time_inc        = CSRStorage(32, reset=self.time_inc.reset, description="Time Increment in ns per tick (Q8.24 format).")

        # # #

        # Enable.
        self.specials += MultiReg(self._control.fields.enable, self.enable)

        # Sync.
        self.specials += MultiReg(self._control.fields.sync_enable, self.sync_enable)

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

        # Time Increment.
        self.specials += MultiReg(self._time_inc.storage, self.time_inc, "time")

    def add_cdc(self):
        time        = Signal(64)
        time_change = Signal()
        cdc_layout = [
            ("time",   64),
            ("change",  1),
        ]
        self.cdc = cdc = stream.ClockDomainCrossing(
            layout     = cdc_layout,
            cd_from    = "time",
            cd_to      = "sys",
            with_common_rst = True,
        )
        self.comb += [
            cdc.sink.valid.eq(1),
            cdc.sink.time.eq(self.time),
            cdc.sink.change.eq(self.time_change),
            cdc.source.ready.eq(1),
        ]
        self.sync += If(cdc.source.valid,
            time.eq(        cdc.source.time),
            time_change.eq( cdc.source.change),
        )
        self.time        = time
        self.time_change = time_change
