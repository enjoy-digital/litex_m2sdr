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

        self.write_ps  = PulseSynchronizer(cd_from, "time")
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

    NS_PER_SECOND      = 1_000_000_000
    SECOND_STEP_MIN_NS = 999_000_000
    SECOND_STEP_MAX_NS = 1_001_000_000

    STATE_MANUAL   = 0
    STATE_ACQUIRE  = 1
    STATE_LOCKED   = 2
    STATE_HOLDOVER = 3

    def __init__(self, sys_clk_freq, nominal_time_inc, with_csr=True):
        self.enable                = Signal(reset=1)
        self.holdover_enable       = Signal(reset=1)
        self.update_cycles_cfg     = Signal(32, reset=max(1, int(sys_clk_freq // 64)))
        self.coarse_threshold_cfg  = Signal(32, reset=100_000)
        self.phase_threshold_cfg   = Signal(32, reset=128)
        self.phase_lock_window_cfg = Signal(32, reset=4_096)
        self.unlock_misses_cfg     = Signal(32, reset=64)
        self.coarse_confirm_cfg    = Signal(32, reset=0)
        self.phase_step_shift_cfg  = Signal(6,  reset=2)
        self.phase_step_max_cfg    = Signal(32, reset=4_096)
        self.trim_limit_cfg        = Signal(32, reset=200_000)
        self.p_gain_cfg            = Signal(16, reset=8)

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

        self.active               = Signal()
        self.locked               = Signal()
        self.holdover             = Signal()
        self.state                = Signal(2, reset=self.STATE_MANUAL)
        self.last_error           = Signal((65, True))
        self.last_ptp_time        = Signal(64)
        self.last_local_time      = Signal(64)
        self.coarse_steps         = Signal(32)
        self.phase_steps          = Signal(32)
        self.rate_updates         = Signal(32)
        self.ptp_lock_losses      = Signal(32)
        self.time_lock_misses     = Signal(32)
        self.time_lock_miss_count = Signal(32)
        self.time_lock_losses     = Signal(32)
        self.clear_counters       = Signal()

        # # #

        sample_counter             = Signal(32)
        ever_locked                = Signal()
        prev_ptp_locked            = Signal()
        prev_time_locked           = Signal()
        integral_trim              = Signal((32, True))
        ptp_seconds_d0             = Signal(34)
        ptp_nanoseconds_d0         = Signal(32)
        ptp_seconds_probe          = Signal(34)
        ptp_nanoseconds_probe      = Signal(32)
        local_time_probe           = Signal(64)
        ptp_locked_probe           = Signal()
        local_time_d0              = Signal(64)
        local_time_d1              = Signal(64)
        ptp_locked_d0              = Signal()
        ptp_locked_d1              = Signal()
        ptp_locked_d2              = Signal()
        ptp_time_base              = Signal(64)
        ptp_time_ns                = Signal(64)
        ptp_time_signed            = Signal((65, True))
        local_time_signed          = Signal((65, True))
        error_ns                   = Signal((65, True))
        error_abs                  = Signal(65)
        phase_shifted              = Signal(65)
        phase_step                 = Signal(64)
        p_term                     = Signal((81, True))
        trim_sum                   = Signal((82, True))
        trim_term                  = Signal((32, True))
        integral_sum               = Signal((66, True))
        integral_next              = Signal((32, True))
        effective_update_cycles    = Signal(32)
        effective_unlock_misses    = Signal(32)
        effective_coarse_confirm   = Signal(32)
        runtime_coarse_enabled     = Signal()
        unlock_miss_limit          = Signal(32)
        coarse_confirm_limit       = Signal(32)
        next_locked                = Signal()
        holdover_allowed           = Signal()
        coarse_error               = Signal()
        time_in_lock_window        = Signal()
        runtime_coarse             = Signal()
        runtime_coarse_countable   = Signal()
        runtime_coarse_allowed     = Signal()
        runtime_whole_second_step  = Signal()
        coarse_needed              = Signal()
        coarse_rejected            = Signal()
        phase_needed               = Signal()
        sample_request             = Signal()
        sample_start               = Signal()
        sample_pending             = Signal()
        sample_pipe0               = Signal()
        sample_pipe1               = Signal()
        sample_pipe2               = Signal()
        sample_pipe3               = Signal()
        sample_pipe4               = Signal()
        sample_pipe5               = Signal()
        sample_pipe6               = Signal()
        sample_pipe7               = Signal()
        sample_pipeline_busy       = Signal()

        self.comb += [
            self.discipline_enable.eq(self.enable),
            self.active.eq(self.enable),
            holdover_allowed.eq(self.holdover_enable & ever_locked),
            If(self.update_cycles_cfg == 0,
                effective_update_cycles.eq(1),
            ).Else(
                effective_update_cycles.eq(self.update_cycles_cfg),
            ),
            If(self.unlock_misses_cfg == 0,
                effective_unlock_misses.eq(1),
            ).Else(
                effective_unlock_misses.eq(self.unlock_misses_cfg),
            ),
            If(self.coarse_confirm_cfg == 0,
                effective_coarse_confirm.eq(1),
                runtime_coarse_enabled.eq(0),
            ).Else(
                effective_coarse_confirm.eq(self.coarse_confirm_cfg),
                runtime_coarse_enabled.eq(1),
            ),
            unlock_miss_limit.eq(effective_unlock_misses - 1),
            coarse_confirm_limit.eq(effective_coarse_confirm - 1),
            coarse_error.eq(error_abs >= self.coarse_threshold_cfg),
            time_in_lock_window.eq(error_abs <= self.phase_lock_window_cfg),
            runtime_coarse.eq(self.locked & ever_locked & coarse_error),
            runtime_whole_second_step.eq(
                runtime_coarse &
                (error_abs >= self.SECOND_STEP_MIN_NS) &
                (error_abs <= self.SECOND_STEP_MAX_NS)
            ),
            runtime_coarse_countable.eq(runtime_coarse_enabled & ~runtime_whole_second_step),
            runtime_coarse_allowed.eq(
                runtime_coarse_countable &
                (self.time_lock_miss_count >= coarse_confirm_limit)
            ),
            sample_pipeline_busy.eq(
                sample_pipe0 |
                sample_pipe1 |
                sample_pipe2 |
                sample_pipe3 |
                sample_pipe4 |
                sample_pipe5 |
                sample_pipe6 |
                sample_pipe7
            ),
            sample_request.eq(~sample_pipeline_busy & ~sample_pending & (sample_counter >= (effective_update_cycles - 1))),
            sample_start.eq(sample_pending & (self.ptp_seconds[:34] == ptp_seconds_probe)),
        ]

        # Timestamp Capture.
        # ------------------
        # Wait for a repeated PTP seconds value before accepting a sample, avoiding mixed
        # seconds/nanoseconds reads around a PTP second rollover.
        # Pipeline stages: capture a stable sample, build PTP ns, compute signed error,
        # decide lock/coarse policy, clamp trims, then apply the selected servo action.
        self.sync += [
            sample_pipe0.eq(sample_start),
            sample_pipe1.eq(sample_pipe0),
            sample_pipe2.eq(sample_pipe1),
            sample_pipe3.eq(sample_pipe2),
            sample_pipe4.eq(sample_pipe3),
            sample_pipe5.eq(sample_pipe4),
            sample_pipe6.eq(sample_pipe5),
            sample_pipe7.eq(sample_pipe6),

            If(sample_request,
                sample_pending.eq(1),
                ptp_seconds_probe.eq(self.ptp_seconds[:34]),
                ptp_nanoseconds_probe.eq(self.ptp_nanoseconds),
                local_time_probe.eq(self.local_time),
                ptp_locked_probe.eq(self.ptp_locked),
            ).Elif(sample_pending,
                If(sample_start,
                    sample_pending.eq(0),
                    ptp_seconds_d0.eq(ptp_seconds_probe),
                    ptp_nanoseconds_d0.eq(ptp_nanoseconds_probe),
                    local_time_d0.eq(local_time_probe),
                    ptp_locked_d0.eq(ptp_locked_probe),
                ).Else(
                    ptp_seconds_probe.eq(self.ptp_seconds[:34]),
                    ptp_nanoseconds_probe.eq(self.ptp_nanoseconds),
                    local_time_probe.eq(self.local_time),
                    ptp_locked_probe.eq(self.ptp_locked),
                )
            ),
            If(sample_pipe0,
                ptp_time_base.eq(ptp_seconds_d0 * self.NS_PER_SECOND),
            ),
            If(sample_pipe1,
                ptp_time_ns.eq(ptp_time_base + ptp_nanoseconds_d0),
                local_time_d1.eq(local_time_d0),
                ptp_locked_d1.eq(ptp_locked_d0),
            ),
            If(sample_pipe2,
                ptp_time_signed.eq(ptp_time_ns),
                local_time_signed.eq(local_time_d1),
                ptp_locked_d2.eq(ptp_locked_d1),
            ),
            If(sample_pipe3,
                error_ns.eq(ptp_time_signed - local_time_signed),
            ),
            If(sample_pipe4,
                error_abs.eq(Mux(error_ns < 0, -error_ns, error_ns)),
                p_term.eq(error_ns * self.p_gain_cfg),
            ),
            If(sample_pipe5,
                phase_shifted.eq(error_abs >> self.phase_step_shift_cfg),
                trim_sum.eq(integral_trim + p_term),
                integral_sum.eq(integral_trim + error_ns),
                If(~self.enable,
                    next_locked.eq(0),
                ).Elif(~ptp_locked_d2,
                    next_locked.eq(0),
                ).Elif(~ever_locked,
                    next_locked.eq(0),
                ).Elif(coarse_error,
                    next_locked.eq(self.locked),
                ).Elif(time_in_lock_window,
                    next_locked.eq(1),
                ).Elif(self.locked & (self.time_lock_miss_count < unlock_miss_limit),
                    next_locked.eq(1),
                ).Else(
                    next_locked.eq(0),
                ),
                coarse_rejected.eq(
                    runtime_coarse &
                    ~runtime_coarse_allowed
                ),
                coarse_needed.eq(
                    ~ever_locked |
                    (
                        coarse_error &
                        (~self.locked | runtime_coarse_allowed)
                    )
                ),
                phase_needed.eq(error_abs >= self.phase_threshold_cfg),
            ),
            If(sample_pipe6,
                If(error_abs == 0,
                    phase_step.eq(0),
                ).Elif(phase_shifted == 0,
                    phase_step.eq(1),
                ).Elif(phase_shifted > self.phase_step_max_cfg,
                    phase_step.eq(self.phase_step_max_cfg),
                ).Else(
                    phase_step.eq(phase_shifted),
                ),
                If(trim_sum < -self.trim_limit_cfg,
                    trim_term.eq(-self.trim_limit_cfg),
                ).Elif(trim_sum > self.trim_limit_cfg,
                    trim_term.eq(self.trim_limit_cfg),
                ).Else(
                    trim_term.eq(trim_sum),
                ),
                If(integral_sum < -self.trim_limit_cfg,
                    integral_next.eq(-self.trim_limit_cfg),
                ).Elif(integral_sum > self.trim_limit_cfg,
                    integral_next.eq(self.trim_limit_cfg),
                ).Else(
                    integral_next.eq(integral_sum),
                ),
            ),
        ]

        self.sync += [
            self.discipline_write.eq(0),
            self.discipline_adjust.eq(0),
            If(sample_request,
                sample_counter.eq(0),
            ).Elif(~sample_pipeline_busy & ~sample_pending,
                sample_counter.eq(sample_counter + 1),
            ),
        ]

        # Servo State / Actions.
        # ----------------------
        # Initial acquisition uses a coarse write. Once locked, large runtime errors are
        # deglitched before any coarse realignment; normal samples use bounded trims.
        self.sync += [
            If(self.clear_counters,
                self.coarse_steps.eq(0),
                self.phase_steps.eq(0),
                self.rate_updates.eq(0),
                self.ptp_lock_losses.eq(0),
                self.time_lock_misses.eq(0),
                self.time_lock_miss_count.eq(0),
                self.time_lock_losses.eq(0),
            ).Elif(~self.enable,
                self.discipline_time_inc.eq(nominal_time_inc),
                self.discipline_write_time.eq(0),
                self.discipline_adjust_sign.eq(0),
                self.discipline_adjustment.eq(0),
                self.locked.eq(0),
                self.holdover.eq(0),
                self.state.eq(self.STATE_MANUAL),
                sample_pending.eq(0),
                sample_counter.eq(0),
                ever_locked.eq(0),
                prev_ptp_locked.eq(0),
                prev_time_locked.eq(0),
                self.time_lock_miss_count.eq(0),
                integral_trim.eq(0),
                self.last_error.eq(0),
                self.last_ptp_time.eq(0),
                self.last_local_time.eq(0),
            ).Elif(sample_pipe7,
                If(prev_ptp_locked & ~ptp_locked_d2,
                    self.ptp_lock_losses.eq(self.ptp_lock_losses + 1),
                ),
                If(prev_time_locked & ~next_locked,
                    self.time_lock_losses.eq(self.time_lock_losses + 1),
                ),
                prev_ptp_locked.eq(ptp_locked_d2),
                prev_time_locked.eq(next_locked),
                If(~coarse_rejected,
                    self.last_error.eq(error_ns),
                    self.last_ptp_time.eq(ptp_time_ns),
                    self.last_local_time.eq(local_time_d1),
                ),
                If(ptp_locked_d2,
                    self.holdover.eq(0),
                    If(coarse_rejected,
                        self.locked.eq(next_locked),
                        self.state.eq(Mux(next_locked, self.STATE_LOCKED, self.STATE_ACQUIRE)),
                        ever_locked.eq(1),
                        self.time_lock_misses.eq(self.time_lock_misses + 1),
                        If(runtime_coarse_countable,
                            self.time_lock_miss_count.eq(self.time_lock_miss_count + 1),
                        ).Else(
                            self.time_lock_miss_count.eq(0),
                        ),
                    ).Elif(coarse_needed,
                        self.discipline_write.eq(1),
                        self.discipline_write_time.eq(ptp_time_ns),
                        self.discipline_time_inc.eq(nominal_time_inc),
                        self.discipline_adjust_sign.eq(0),
                        self.discipline_adjustment.eq(0),
                        self.coarse_steps.eq(self.coarse_steps + 1),
                        self.locked.eq(next_locked),
                        self.state.eq(Mux(next_locked, self.STATE_LOCKED, self.STATE_ACQUIRE)),
                        ever_locked.eq(1),
                        self.time_lock_miss_count.eq(0),
                        integral_trim.eq(0),
                    ).Else(
                        If(phase_needed,
                            self.discipline_adjust.eq(1),
                            self.discipline_adjust_sign.eq(error_ns < 0),
                            self.discipline_adjustment.eq(phase_step),
                            self.phase_steps.eq(self.phase_steps + 1),
                        ),
                        integral_trim.eq(integral_next),
                        self.discipline_time_inc.eq(nominal_time_inc + trim_term),
                        self.rate_updates.eq(self.rate_updates + 1),
                        self.locked.eq(next_locked),
                        self.state.eq(Mux(next_locked, self.STATE_LOCKED, self.STATE_ACQUIRE)),
                        ever_locked.eq(1),
                        If(time_in_lock_window,
                            self.time_lock_miss_count.eq(0),
                        ).Elif(self.locked & next_locked,
                            self.time_lock_misses.eq(self.time_lock_misses + 1),
                            self.time_lock_miss_count.eq(self.time_lock_miss_count + 1),
                        ).Else(
                            self.time_lock_miss_count.eq(0),
                        ),
                    )
                ).Else(
                    self.locked.eq(0),
                    self.time_lock_miss_count.eq(0),
                    If(holdover_allowed,
                        self.holdover.eq(1),
                        self.state.eq(self.STATE_HOLDOVER),
                    ).Else(
                        self.holdover.eq(0),
                        self.state.eq(self.STATE_ACQUIRE),
                        self.discipline_time_inc.eq(nominal_time_inc),
                        self.discipline_adjust_sign.eq(0),
                        self.discipline_adjustment.eq(0),
                        integral_trim.eq(0),
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
            CSRField("holdover", size=1, offset=1, reset=1, values=[
                ("``0b0``", "Drop back to acquire mode when PTP lock is lost."),
                ("``0b1``", "Keep the last disciplined rate while waiting for PTP lock to return."),
            ]),
        ])
        self._clear_counters = CSRStorage(description="Write to clear the PTP discipline counters.")
        self._update_cycles = CSRStorage(32,
            reset=self.update_cycles_cfg.reset.value,
            description="Servo update interval in sys_clk cycles."
        )
        self._coarse_threshold = CSRStorage(32,
            reset=self.coarse_threshold_cfg.reset.value,
            description="Coarse realignment threshold in nanoseconds."
        )
        self._phase_threshold = CSRStorage(32,
            reset=self.phase_threshold_cfg.reset.value,
            description="Bounded phase-trim threshold in nanoseconds."
        )
        self._lock_window = CSRStorage(32,
            reset=self.phase_lock_window_cfg.reset.value,
            description="Time-lock window in nanoseconds."
        )
        self._unlock_misses = CSRStorage(32,
            reset=self.unlock_misses_cfg.reset.value,
            description="Consecutive out-of-window samples required before dropping time lock."
        )
        self._coarse_confirm = CSRStorage(32,
            reset=self.coarse_confirm_cfg.reset.value,
            description="Consecutive coarse-error samples required before runtime board-time realignment; "
                        "0 disables runtime coarse realignment."
        )
        self._phase_step_shift = CSRStorage(6,
            reset=self.phase_step_shift_cfg.reset.value,
            description="Right shift applied to the absolute phase error before bounded trims."
        )
        self._phase_step_max = CSRStorage(32,
            reset=self.phase_step_max_cfg.reset.value,
            description="Maximum bounded phase trim in nanoseconds."
        )
        self._trim_limit = CSRStorage(32,
            reset=self.trim_limit_cfg.reset.value,
            description="Maximum frequency trim in time_inc LSB units."
        )
        self._p_gain = CSRStorage(16,
            reset=self.p_gain_cfg.reset.value,
            description="Proportional gain applied to the phase error before rate trim limiting."
        )

        self._status = CSRStatus(fields=[
            CSRField("enable",      size=1, offset=0, description="PTP time discipline enabled."),
            CSRField("active",      size=1, offset=1, description="PTP discipline currently owns the board time generator."),
            CSRField("ptp_locked",  size=1, offset=2, description="LiteEth PTP core is locked to a master."),
            CSRField("time_locked", size=1, offset=3, description="Board time is within the lock window of PTP time."),
            CSRField("holdover",    size=1, offset=4, description="PTP lock was lost; keeping the last disciplined rate."),
            CSRField("state",       size=2, offset=8, values=[
                ("``0b00``", "Manual/free-run."),
                ("``0b01``", "Acquire."),
                ("``0b10``", "Locked."),
                ("``0b11``", "Holdover."),
            ], description="PTP time discipline state."),
        ])
        self._time_inc = CSRStatus(32,
            description="Disciplined TimeGenerator increment (Q8.24 ns/tick)."
        )
        self._last_error = CSRStatus(64,
            description="Last signed PTP minus board-time error (ns, two's complement)."
        )
        self._last_ptp_time = CSRStatus(64,
            description="Last sampled PTP time (ns)."
        )
        self._last_local_time = CSRStatus(64,
            description="Last sampled board time (ns)."
        )
        self._coarse_steps = CSRStatus(32,
            description="Number of coarse board-time realignments."
        )
        self._phase_steps = CSRStatus(32,
            description="Number of bounded phase trims."
        )
        self._rate_updates = CSRStatus(32,
            description="Number of rate trim updates."
        )
        self._ptp_lock_losses = CSRStatus(32,
            description="Number of PTP lock-loss events observed by the discipline loop."
        )
        self._time_lock_misses = CSRStatus(32,
            description="Number of out-of-window samples tolerated while time lock was held."
        )
        self._time_lock_miss_count = CSRStatus(32,
            description="Current consecutive out-of-window sample count while time lock is held."
        )
        self._time_lock_losses = CSRStatus(32,
            description="Number of deglitched board-time lock-loss events observed by the discipline loop."
        )

        self.comb += [
            self.enable.eq(self._control.fields.enable),
            self.holdover_enable.eq(self._control.fields.holdover),
            self.clear_counters.eq(self._clear_counters.re),
            self.update_cycles_cfg.eq(self._update_cycles.storage),
            self.coarse_threshold_cfg.eq(self._coarse_threshold.storage),
            self.phase_threshold_cfg.eq(self._phase_threshold.storage),
            self.phase_lock_window_cfg.eq(self._lock_window.storage),
            self.unlock_misses_cfg.eq(self._unlock_misses.storage),
            self.coarse_confirm_cfg.eq(self._coarse_confirm.storage),
            self.phase_step_shift_cfg.eq(self._phase_step_shift.storage),
            self.phase_step_max_cfg.eq(self._phase_step_max.storage),
            self.trim_limit_cfg.eq(self._trim_limit.storage),
            self.p_gain_cfg.eq(self._p_gain.storage),
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
            self._ptp_lock_losses.status.eq(self.ptp_lock_losses),
            self._time_lock_misses.status.eq(self.time_lock_misses),
            self._time_lock_miss_count.status.eq(self.time_lock_miss_count),
            self._time_lock_losses.status.eq(self.time_lock_losses),
        ]
