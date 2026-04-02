#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *

# Time Discipline CDC ------------------------------------------------------------------------------

class TimeDisciplineCDC(LiteXModule):
    """Bridge sys-domain time discipline commands to the time domain."""
    def __init__(self, time_gen, cd_from="sys"):
        self.enable      = Signal()
        self.time_inc    = Signal(32)
        self.write       = Signal()
        self.write_time  = Signal(64)
        self.adjust      = Signal()
        self.adjust_sign = Signal()
        self.adjustment  = Signal(64)

        # # #

        self.specials += [
            MultiReg(self.enable,      time_gen.discipline_enable,      "time"),
            MultiReg(self.time_inc,    time_gen.discipline_time_inc,    "time"),
            MultiReg(self.write_time,  time_gen.discipline_write_time,  "time"),
            MultiReg(self.adjust_sign, time_gen.discipline_adjust_sign, "time"),
            MultiReg(self.adjustment,  time_gen.discipline_adjustment,  "time"),
        ]

        self.write_ps = PulseSynchronizer(cd_from, "time")
        self.adjust_ps = PulseSynchronizer(cd_from, "time")
        self.submodules += self.write_ps, self.adjust_ps
        self.comb += [
            self.write_ps.i.eq(self.write),
            self.adjust_ps.i.eq(self.adjust),
            time_gen.discipline_write.eq(self.write_ps.o),
            time_gen.discipline_adjust.eq(self.adjust_ps.o),
        ]

# PTP Time Discipline ------------------------------------------------------------------------------

class PTPTimeDiscipline(LiteXModule):
    """Discipline the SI5351-backed board time from LiteEth PTP time."""

    STATE_MANUAL  = 0
    STATE_ACQUIRE = 1
    STATE_LOCKED  = 2
    STATE_HOLDOVER = 3

    def __init__(self, sys_clk_freq, nominal_time_inc, with_csr=True):
        self.enable          = Signal(reset=1)
        self.local_time      = Signal(64)
        self.ptp_seconds     = Signal(48)
        self.ptp_nanoseconds = Signal(32)
        self.ptp_locked      = Signal()

        self.discipline_enable      = Signal()
        self.discipline_time_inc    = Signal(32, reset=nominal_time_inc)
        self.discipline_write       = Signal()
        self.discipline_write_time  = Signal(64)
        self.discipline_adjust      = Signal()
        self.discipline_adjust_sign = Signal()
        self.discipline_adjustment  = Signal(64)

        self.active        = Signal()
        self.locked        = Signal()
        self.holdover      = Signal()
        self.state         = Signal(2, reset=self.STATE_MANUAL)
        self.last_error    = Signal((65, True))
        self.last_ptp_time = Signal(64)
        self.last_local_time = Signal(64)
        self.coarse_steps  = Signal(32)
        self.phase_steps   = Signal(32)
        self.rate_updates  = Signal(32)

        # # #

        update_cycles      = max(1, int(sys_clk_freq // 64))
        coarse_threshold   = 100_000
        phase_threshold    = 128
        phase_lock_window  = 500
        phase_step_max     = 4_096
        trim_limit         = 200_000
        p_gain             = 8
        seconds_to_ns      = 1_000_000_000

        sample_counter     = Signal(max=max(update_cycles + 1, 2))
        sample_tick        = Signal()
        ever_locked        = Signal()
        integral_trim      = Signal((32, True))
        ptp_time_ns        = Signal(64)
        ptp_time_signed    = Signal((65, True))
        local_time_signed  = Signal((65, True))
        error_ns           = Signal((65, True))
        error_abs          = Signal(65)
        phase_step         = Signal(64)
        p_term             = Signal((65, True))
        trim_sum           = Signal((66, True))
        trim_term          = Signal((32, True))

        self.comb += [
            self.discipline_enable.eq(self.enable),
            self.active.eq(self.enable),
            ptp_time_ns.eq((self.ptp_seconds[:34] * seconds_to_ns) + self.ptp_nanoseconds),
            ptp_time_signed.eq(ptp_time_ns),
            local_time_signed.eq(self.local_time),
            error_ns.eq(ptp_time_signed - local_time_signed),
            error_abs.eq(Mux(error_ns < 0, -error_ns, error_ns)),
            p_term.eq(error_ns * p_gain),
            trim_sum.eq(integral_trim + p_term),
            If(error_abs == 0,
                phase_step.eq(0),
            ).Elif((error_abs >> 2) == 0,
                phase_step.eq(1),
            ).Elif((error_abs >> 2) > phase_step_max,
                phase_step.eq(phase_step_max),
            ).Else(
                phase_step.eq(error_abs >> 2),
            ),
        ]

        self.sync += [
            self.discipline_write.eq(0),
            self.discipline_adjust.eq(0),
            If(sample_counter == (update_cycles - 1),
                sample_counter.eq(0),
                sample_tick.eq(1),
            ).Else(
                sample_counter.eq(sample_counter + 1),
                sample_tick.eq(0),
            ),
        ]

        self.sync += [
            If(~self.enable,
                self.discipline_time_inc.eq(nominal_time_inc),
                self.discipline_write_time.eq(0),
                self.discipline_adjust_sign.eq(0),
                self.discipline_adjustment.eq(0),
                self.locked.eq(0),
                self.holdover.eq(0),
                self.state.eq(self.STATE_MANUAL),
                ever_locked.eq(0),
                integral_trim.eq(0),
                self.last_error.eq(0),
                self.last_ptp_time.eq(0),
                self.last_local_time.eq(0),
            ).Elif(sample_tick,
                self.last_error.eq(error_ns),
                self.last_ptp_time.eq(ptp_time_ns),
                self.last_local_time.eq(self.local_time),
                If(self.ptp_locked,
                    self.holdover.eq(0),
                    If((error_abs >= coarse_threshold) | ~ever_locked,
                        self.discipline_write.eq(1),
                        self.discipline_write_time.eq(ptp_time_ns),
                        self.discipline_time_inc.eq(nominal_time_inc),
                        self.discipline_adjust_sign.eq(0),
                        self.discipline_adjustment.eq(0),
                        self.coarse_steps.eq(self.coarse_steps + 1),
                        self.locked.eq(0),
                        self.state.eq(self.STATE_ACQUIRE),
                        ever_locked.eq(1),
                        integral_trim.eq(0),
                    ).Else(
                        If(error_abs >= phase_threshold,
                            self.discipline_adjust.eq(1),
                            self.discipline_adjust_sign.eq(error_ns < 0),
                            self.discipline_adjustment.eq(phase_step),
                            self.phase_steps.eq(self.phase_steps + 1),
                        ),
                        If(trim_sum < -trim_limit,
                            trim_term.eq(-trim_limit),
                        ).Elif(trim_sum > trim_limit,
                            trim_term.eq(trim_limit),
                        ).Else(
                            trim_term.eq(trim_sum),
                        ),
                        If((integral_trim + error_ns) < -trim_limit,
                            integral_trim.eq(-trim_limit),
                        ).Elif((integral_trim + error_ns) > trim_limit,
                            integral_trim.eq(trim_limit),
                        ).Else(
                            integral_trim.eq(integral_trim + error_ns),
                        ),
                        self.discipline_time_inc.eq(nominal_time_inc + trim_term),
                        self.rate_updates.eq(self.rate_updates + 1),
                        self.locked.eq(error_abs <= phase_lock_window),
                        self.state.eq(Mux(error_abs <= phase_lock_window, self.STATE_LOCKED, self.STATE_ACQUIRE)),
                        ever_locked.eq(1),
                    )
                ).Else(
                    self.locked.eq(0),
                    If(ever_locked,
                        self.holdover.eq(1),
                        self.state.eq(self.STATE_HOLDOVER),
                    ).Else(
                        self.holdover.eq(0),
                        self.state.eq(self.STATE_ACQUIRE),
                    ),
                )
            )
        ]

        if with_csr:
            self.add_csr()

    def add_csr(self):
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, reset=1, values=[
                ("``0b0``", "Disable PTP ownership of the board time generator."),
                ("``0b1``", "Discipline the board time generator from Ethernet PTP."),
            ]),
        ])
        self._status = CSRStatus(fields=[
            CSRField("enable",     size=1, offset=0, description="PTP time discipline enabled."),
            CSRField("active",     size=1, offset=1, description="PTP discipline currently owns the board time generator."),
            CSRField("ptp_locked", size=1, offset=2, description="LiteEth PTP core is locked to a master."),
            CSRField("time_locked",size=1, offset=3, description="Board time is within the lock window of PTP time."),
            CSRField("holdover",   size=1, offset=4, description="PTP lock was lost; keeping the last disciplined rate."),
            CSRField("state",      size=2, offset=8, description="0=manual, 1=acquire, 2=locked, 3=holdover."),
        ])
        self._time_inc       = CSRStatus(32, description="Disciplined TimeGenerator increment (Q8.24 ns/tick).")
        self._last_error     = CSRStatus(64, description="Last signed PTP minus board-time error (ns, two's complement).")
        self._last_ptp_time  = CSRStatus(64, description="Last sampled PTP time (ns).")
        self._last_local_time = CSRStatus(64, description="Last sampled board time (ns).")
        self._coarse_steps   = CSRStatus(32, description="Number of coarse board-time realignments.")
        self._phase_steps    = CSRStatus(32, description="Number of bounded phase trims.")
        self._rate_updates   = CSRStatus(32, description="Number of rate trim updates.")

        self.specials += MultiReg(self._control.fields.enable, self.enable)
        self.comb += [
            self._status.fields.enable.eq(self.enable),
            self._status.fields.active.eq(self.active),
            self._status.fields.ptp_locked.eq(self.ptp_locked),
            self._status.fields.time_locked.eq(self.locked),
            self._status.fields.holdover.eq(self.holdover),
            self._status.fields.state.eq(self.state),
            self._time_inc.status.eq(self.discipline_time_inc),
            self._last_error.status.eq(self.last_error[0:64]),
            self._last_ptp_time.status.eq(self.last_ptp_time),
            self._last_local_time.status.eq(self.last_local_time),
            self._coarse_steps.status.eq(self.coarse_steps),
            self._phase_steps.status.eq(self.phase_steps),
            self._rate_updates.status.eq(self.rate_updates),
        ]
