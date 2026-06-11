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

from litex_m2sdr.gateware.cdc import ValueStrobeCDC

# Time Generator -----------------------------------------------------------------------------------

class TimeGenerator(LiteXModule):
    def __init__(self, clk, clk_freq, init=0, with_csr=True):
        time_inc_reset = int(round((1e9 / clk_freq) * (1 << 24)))
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

        # Time Discipline Interface (time domain).
        self.discipline_enable      = Signal()
        self.discipline_time_inc    = Signal(32, reset=time_inc_reset)
        self.discipline_write       = Signal()
        self.discipline_write_time  = Signal(64)
        self.discipline_adjust      = Signal()
        self.discipline_adjust_sign = Signal()
        self.discipline_adjustment  = Signal(64)

        # Time Sync Interface.
        self.time_sync    = Signal()
        self.time_seconds = Signal(40)

        # # #

        # Time Signals.
        time = Signal(64, reset=init)
        frac = Signal(24)

        # Time Clk Domain.
        self.cd_time = ClockDomain(reset_less=True)
        self.comb += self.cd_time.clk.eq(clk)

        # Helpers.
        effective_time_inc  = Signal(32)
        adjust_with_underflow = Signal()
        self._time_adjust_change = Signal()
        self.comb += [
            effective_time_inc.eq(Mux(self.discipline_enable, self.discipline_time_inc, self.time_inc)),
            adjust_with_underflow.eq(self.discipline_adjust_sign & (time < self.discipline_adjustment)),
        ]

        # Time Handling (Q8.24 fractional ns).
        # Priority is explicit: reset, coarse write, PPS sync, bounded phase trim, then
        # normal fractional increment. The PTP discipline uses the same write/trim path
        # as software so time changes remain centralized in this clock domain.
        self.sync.time += [
            If(~self.enable,
                time.eq(init),
                frac.eq(0),
            # Software / external coarse write.
            ).Elif(self.write | self.discipline_write,
                time.eq(Mux(self.discipline_write, self.discipline_write_time, self.write_time)),
                frac.eq(0),
            # Time Sync.
            ).Elif(self.sync_enable & self.time_sync,
                time.eq(((self.time_seconds + 1) * int(1e9))),
                frac.eq(0),
            # External phase trim.
            ).Elif(self.discipline_adjust,
                If(adjust_with_underflow,
                    time.eq(0),
                ).Elif(self.discipline_adjust_sign,
                    time.eq(time - self.discipline_adjustment),
                ).Else(
                    time.eq(time + self.discipline_adjustment),
                ),
            # Increment (fractional).
            ).Else(
                Cat(frac, time).eq(Cat(frac, time) + effective_time_inc),
            ),
            self.time.eq(time + self.time_adjustment),
            self.time_change.eq(
                self.write |
                self.discipline_write |
                self._time_adjust_change |
                (self.sync_enable & self.time_sync)
            )
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
        self._time_inc        = CSRStorage(32,
            reset=self.time_inc.reset,
            description="Time Increment in ns per tick (Q8.24 format)."
        )

        # # #

        # Enable.
        self.specials += MultiReg(self._control.fields.enable, self.enable)

        # Sync.
        self.specials += MultiReg(self._control.fields.sync_enable, self.sync_enable)

        # Time Read (FPGA -> SW). The full 64-bit snapshot crosses through one
        # handshaked crossing: per-bit synchronizers would let software observe
        # torn values while the snapshot settles (the C library used to need a
        # multi-attempt stable-read workaround for exactly this).
        time_read_ps = PulseSynchronizer("sys", "time")
        self.submodules += time_read_ps
        self.comb += time_read_ps.i.eq(self._control.fields.read)
        self.time_read_cdc = time_read_cdc = ValueStrobeCDC(64, cd_from="time", cd_to="sys")
        self.comb += [
            time_read_cdc.strobe.eq(time_read_ps.o),
            time_read_cdc.value.eq(self.time),
        ]
        self.sync += If(time_read_cdc.strobe_o,
            self._read_time.status.eq(time_read_cdc.value_o)
        )

        # Time Write (SW -> FPGA). The value travels with the strobe so the
        # time domain never applies a half-settled 64-bit word.
        self.time_write_cdc = time_write_cdc = ValueStrobeCDC(64, cd_from="sys", cd_to="time")
        self.comb += [
            time_write_cdc.strobe.eq(self._control.fields.write),
            time_write_cdc.value.eq(self._write_time.storage),
            self.write.eq(time_write_cdc.strobe_o),
            self.write_time.eq(time_write_cdc.value_o),
        ]

        # Time Adjust (SW -> FPGA). Registered atomically in the time domain:
        # the adjustment is summed into the live time output on every cycle,
        # so a torn update would glitch the timestamps fed to the DMA headers
        # and VRT packets.
        self.time_adjust_cdc = time_adjust_cdc = ValueStrobeCDC(64, cd_from="sys", cd_to="time")
        self.comb += [
            time_adjust_cdc.strobe.eq(self._time_adjustment.re),
            time_adjust_cdc.value.eq(self._time_adjustment.storage),
            self._time_adjust_change.eq(time_adjust_cdc.strobe_o),
        ]
        self.sync.time += If(time_adjust_cdc.strobe_o,
            self.time_adjustment.eq(time_adjust_cdc.value_o)
        )

        # Time Increment (SW -> FPGA). Registered atomically: the increment is
        # accumulated every cycle, so torn bits would permanently offset the
        # time.
        self.time_inc_cdc = time_inc_cdc = ValueStrobeCDC(32, cd_from="sys", cd_to="time")
        self.comb += [
            time_inc_cdc.strobe.eq(self._time_inc.re),
            time_inc_cdc.value.eq(self._time_inc.storage),
        ]
        self.sync.time += If(time_inc_cdc.strobe_o,
            self.time_inc.eq(time_inc_cdc.value_o)
        )

    def add_cdc(self):
        time        = Signal(64)
        time_change = Signal()
        cdc_layout = [
            ("time",   64),
            ("change",  1),
        ]
        self.cdc = cdc = stream.ClockDomainCrossing(
            layout          = cdc_layout,
            cd_from         = "time",
            cd_to           = "sys",
        )
        self.comb += [
            cdc.sink.valid.eq(1),
            cdc.sink.time.eq(self.time),
            cdc.sink.change.eq(self.time_change),
            cdc.source.ready.eq(1),
        ]
        self.sync += If(cdc.source.valid,
            time.eq(       cdc.source.time),
            time_change.eq(cdc.source.change),
        )
        self.time        = time
        self.time_change = time_change


class TimeNsToPS(LiteXModule):
    """Convert nanoseconds timestamp to (seconds, picosecond-fraction) fields for VRT TSF=REAL_TIME."""
    def __init__(self, time_ns, time_s, time_ps):
        assert len(time_ns) == 64
        assert len(time_s) == 32
        assert len(time_ps) == 64

        time_ns_reg = Signal(64)
        product_s1e9 = Signal(64)
        remainder_ns = Signal(32)
        fraction_ps = Signal(64)

        self.sync += [
            time_ns_reg.eq(time_ns),
            product_s1e9.eq(time_s * 1_000_000_000),
            remainder_ns.eq(time_ns_reg - product_s1e9),
            fraction_ps.eq(remainder_ns * 1000),
        ]
        self.comb += time_ps.eq(fraction_ps)
