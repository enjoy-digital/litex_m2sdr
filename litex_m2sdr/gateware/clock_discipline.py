#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *

# MMCM Phase Discipline -----------------------------------------------------------------------------

class MMCMPhaseDiscipline(LiteXModule):
    """Drive a MMCM Dynamic Phase Shift port with bounded phase-step requests."""
    def __init__(self, sys_clk_freq, psclk_domain="sys", with_csr=True):
        # Config.
        # -------
        self.enable             = Signal()
        self.update_cycles_cfg  = Signal(32, reset=max(1, int(sys_clk_freq // 1_000_000)))
        self.rate_cfg           = Signal((32, True))
        self.manual_inc         = Signal()
        self.manual_dec         = Signal()
        self.clear_counters     = Signal()

        self.override               = Signal()
        self.override_enable        = Signal()
        self.override_rate          = Signal((32, True))
        self.override_update_cycles = Signal(32, reset=max(1, int(sys_clk_freq // 1_000_000)))

        # MMCM DPS Interface.
        # -------------------
        self.psdone             = Signal()
        self.psen               = Signal()
        self.psincdec           = Signal()

        # Status.
        # -------
        self.busy               = Signal()
        self.pending            = Signal()
        self.last_direction     = Signal()
        self.step_count         = Signal(32)
        self.inc_count          = Signal(32)
        self.dec_count          = Signal(32)
        self.dropped_count      = Signal(32)

        # # #

        # Helpers.
        # --------
        sample_counter          = Signal(32)
        sample_tick             = Signal()
        accumulator             = Signal(32)
        selected_update_cycles  = Signal(32)
        effective_update_cycles = Signal(32)
        effective_enable        = Signal()
        effective_rate          = Signal((32, True))
        rate_negative           = Signal()
        rate_abs                = Signal(32)
        accumulator_sum         = Signal(33)
        auto_step               = Signal()
        step_inc_request        = Signal()
        step_dec_request        = Signal()
        psdone_sys              = Signal()
        psen_sys                = Signal()
        psincdec_sys            = Signal()

        self.comb += [
            effective_enable.eq(Mux(self.override, self.override_enable, self.enable)),
            effective_rate.eq(Mux(self.override, self.override_rate, self.rate_cfg)),
            selected_update_cycles.eq(Mux(self.override, self.override_update_cycles, self.update_cycles_cfg)),
            If(selected_update_cycles == 0,
                effective_update_cycles.eq(1),
            ).Else(
                effective_update_cycles.eq(selected_update_cycles),
            ),
            rate_negative.eq(effective_rate < 0),
            If(effective_rate < 0,
                rate_abs.eq(-effective_rate),
            ).Else(
                rate_abs.eq(effective_rate),
            ),
            accumulator_sum.eq(accumulator + rate_abs),
            auto_step.eq(effective_enable & sample_tick & accumulator_sum[32]),
            step_inc_request.eq(self.manual_inc | (auto_step & ~rate_negative)),
            step_dec_request.eq(self.manual_dec | (auto_step &  rate_negative)),
            self.pending.eq(step_inc_request | step_dec_request),
        ]

        # MMCM DPS Clock-Domain Crossing.
        # -------------------------------
        # The MMCM samples PSEN/PSINCDEC on the PSCLK domain configured through
        # S7MMCM.expose_dps().  Keep the servo, counters and CSRs in sys, then
        # cross only the one-cycle step pulse, the stable direction bit and the
        # PSDONE pulse.  This avoids timing-analyzed sys->PSCLK paths in PCIe
        # builds, where sys normally runs at 125MHz while PSCLK is 200MHz.
        if psclk_domain == "sys":
            self.comb += [
                self.psen.eq(psen_sys),
                self.psincdec.eq(psincdec_sys),
                psdone_sys.eq(self.psdone),
            ]
        else:
            request_cdc = PulseSynchronizer("sys", psclk_domain)
            done_cdc    = PulseSynchronizer(psclk_domain, "sys")
            psincdec_ps = Signal()
            psen_ps     = Signal()
            self.submodules += request_cdc, done_cdc
            self.specials += MultiReg(psincdec_sys, psincdec_ps, odomain=psclk_domain)
            self.comb += [
                request_cdc.i.eq(psen_sys),
                done_cdc.i.eq(self.psdone),
                self.psincdec.eq(psincdec_ps),
                psdone_sys.eq(done_cdc.o),
            ]
            psclk_sync = getattr(self.sync, psclk_domain)
            psclk_sync += [
                psen_ps.eq(request_cdc.o),
                self.psen.eq(psen_ps),
            ]

        # Q0.32 Rate Accumulator.
        # -----------------------
        self.sync += [
            psen_sys.eq(0),
            If(sample_counter >= (effective_update_cycles - 1),
                sample_counter.eq(0),
                sample_tick.eq(1),
            ).Else(
                sample_counter.eq(sample_counter + 1),
                sample_tick.eq(0),
            ),
            If(sample_tick,
                If(effective_enable,
                    accumulator.eq(accumulator_sum[:32]),
                ).Else(
                    accumulator.eq(0),
                )
            )
        ]

        # MMCM DPS Requests.
        # ------------------
        self.sync += [
            If(self.clear_counters,
                self.step_count.eq(0),
                self.inc_count.eq(0),
                self.dec_count.eq(0),
                self.dropped_count.eq(0),
            ),
            If(self.busy,
                If(psdone_sys,
                    self.busy.eq(0),
                    self.step_count.eq(self.step_count + 1),
                    If(self.last_direction,
                        self.inc_count.eq(self.inc_count + 1),
                    ).Else(
                        self.dec_count.eq(self.dec_count + 1),
                    ),
                ).Elif(step_inc_request | step_dec_request,
                    self.dropped_count.eq(self.dropped_count + 1),
                ),
            ).Else(
                If(step_inc_request,
                    psen_sys.eq(1),
                    psincdec_sys.eq(1),
                    self.last_direction.eq(1),
                    self.busy.eq(1),
                ).Elif(step_dec_request,
                    psen_sys.eq(1),
                    psincdec_sys.eq(0),
                    self.last_direction.eq(0),
                    self.busy.eq(1),
                )
            )
        ]

        if with_csr:
            self.add_csr()

    def add_csr(self):
        self._control = CSRStorage(fields=[
            CSRField("enable",         size=1, offset=0, values=[
                ("``0b0``", "MMCM fine phase discipline disabled."),
                ("``0b1``", "MMCM fine phase discipline enabled."),
            ]),
            CSRField("manual_inc",     size=1, offset=1, pulse=True,
                description="Request one positive MMCM fine phase step."),
            CSRField("manual_dec",     size=1, offset=2, pulse=True,
                description="Request one negative MMCM fine phase step."),
            CSRField("clear_counters", size=1, offset=8, pulse=True,
                description="Clear MMCM phase discipline counters."),
        ])
        self._rate = CSRStorage(32,
            description="Signed MMCM fine phase-step rate (two's complement, Q0.32 steps per update interval)."
        )
        self._update_cycles = CSRStorage(32,
            reset=self.update_cycles_cfg.reset,
            description="Number of sys_clk cycles between MMCM fine phase discipline updates."
        )
        self._status = CSRStatus(fields=[
            CSRField("busy",           size=1, offset=0,
                description="MMCM fine phase shift request in progress."),
            CSRField("pending",        size=1, offset=1,
                description="A new MMCM fine phase shift request is currently being presented."),
            CSRField("last_direction", size=1, offset=2, values=[
                ("``0b0``", "Last completed/requested step was negative."),
                ("``0b1``", "Last completed/requested step was positive."),
            ], description="Direction of the most recent MMCM fine phase step."),
        ])
        self._step_count = CSRStatus(32,
            description="Number of completed MMCM fine phase steps."
        )
        self._inc_count = CSRStatus(32,
            description="Number of completed positive MMCM fine phase steps."
        )
        self._dec_count = CSRStatus(32,
            description="Number of completed negative MMCM fine phase steps."
        )
        self._dropped_count = CSRStatus(32,
            description="Number of MMCM fine phase step requests dropped while the MMCM DPS port was busy."
        )

        self.comb += [
            self.enable.eq(self._control.fields.enable),
            self.manual_inc.eq(self._control.fields.manual_inc),
            self.manual_dec.eq(self._control.fields.manual_dec),
            self.clear_counters.eq(self._control.fields.clear_counters),
            self.rate_cfg.eq(self._rate.storage),
            self.update_cycles_cfg.eq(self._update_cycles.storage),
            self._status.fields.busy.eq(self.busy),
            self._status.fields.pending.eq(self.pending),
            self._status.fields.last_direction.eq(self.last_direction),
            self._step_count.status.eq(self.step_count),
            self._inc_count.status.eq(self.inc_count),
            self._dec_count.status.eq(self.dec_count),
            self._dropped_count.status.eq(self.dropped_count),
        ]


# PTP Clock10 Discipline ---------------------------------------------------------------------------

class PTPClock10Discipline(LiteXModule):
    """Monitor and optionally phase-steer the FPGA 10MHz clock against PTP PPS."""

    def __init__(self, sys_clk_freq, clk10, reference_pulse, reference_valid, clk10_freq=10e6, with_csr=True):
        # Config.
        # -------
        self.enable                = Signal(reset=0)
        self.holdover_enable       = Signal(reset=1)
        self.invert                = Signal(reset=0)
        self.manual_align          = Signal()
        self.clear_counters        = Signal()
        self.update_cycles_cfg     = Signal(32, reset=max(1, int(sys_clk_freq // 5_000_000)))
        self.p_gain_cfg            = Signal(32, reset=0x00040000)
        self.i_gain_cfg            = Signal(32, reset=0x00010000)
        self.rate_limit_cfg        = Signal(32, reset=0x7fffffff)
        self.lock_window_ticks_cfg = Signal(32, reset=max(1, int(sys_clk_freq * 50e-6)))
        self.half_period_ticks_cfg = Signal(32, reset=max(1, int(sys_clk_freq // 2)))

        # MMCMPhaseDiscipline override interface.
        # ---------------------------------------
        self.mmcm_override       = Signal()
        self.mmcm_enable         = Signal()
        self.mmcm_rate           = Signal((32, True))
        self.mmcm_update_cycles  = Signal(32)

        # Status.
        # -------
        self.active              = Signal()
        self.reference_locked    = Signal()
        self.locked              = Signal()
        self.holdover            = Signal()
        self.aligned             = Signal()
        self.waiting_after_ref   = Signal()
        self.rate_limited        = Signal()
        self.last_error_ticks    = Signal((32, True))
        self.last_error_ns       = Signal((64, True))
        self.last_rate           = Signal((32, True))
        self.sample_count        = Signal(32)
        self.reference_count     = Signal(32)
        self.clk10_count         = Signal(32)
        self.missing_count       = Signal(32)
        self.align_count         = Signal(32)
        self.lock_loss_count     = Signal(32)
        self.rate_update_count   = Signal(32)
        self.saturation_count    = Signal(32)

        # # #

        sys_clk_period_ns = max(1, int(round(1e9 / sys_clk_freq)))
        clk10_divider     = max(1, int(round(clk10_freq)))

        # clk10-domain 1Hz marker, resettable from sys.
        # ---------------------------------------------
        self.cd_clk10_mon = ClockDomain(reset_less=True)
        self.comb += self.cd_clk10_mon.clk.eq(clk10)

        align_clk10 = Signal()
        clk10_pulse = Signal()
        clk10_counter = Signal(max=max(clk10_divider, 2))

        self.align_ps = PulseSynchronizer("sys", "clk10_mon")
        self.clk10_ps = PulseSynchronizer("clk10_mon", "sys")
        self.submodules += self.align_ps, self.clk10_ps
        self.comb += [
            self.align_ps.i.eq(align_clk10),
            self.clk10_ps.i.eq(clk10_pulse),
        ]

        self.sync.clk10_mon += [
            clk10_pulse.eq(0),
            If(self.align_ps.o,
                clk10_counter.eq(0),
            ).Elif(clk10_counter >= (clk10_divider - 1),
                clk10_counter.eq(0),
                clk10_pulse.eq(1),
            ).Else(
                clk10_counter.eq(clk10_counter + 1),
            )
        ]

        # Phase detector.
        # ---------------
        ticks_since_ref       = Signal(32)
        ticks_since_clk10     = Signal(32)
        have_clk10            = Signal()
        aligned_once          = Signal()
        align_pending         = Signal()
        ever_locked           = Signal()
        prev_locked           = Signal()
        sample_valid          = Signal()
        sample_error_ticks    = Signal((32, True))
        sample_abs_ticks      = Signal(32)
        sample_in_lock_window = Signal()
        rate_multiply_pending = Signal()
        rate_limit_pending    = Signal()
        rate_apply_pending    = Signal()
        rate_error_abs        = Signal(32)
        rate_negative_base    = Signal()
        p_product             = Signal(64)
        p_product_high        = Signal(32)
        p_product_low         = Signal(32)
        p_over_limit          = Signal()
        p_magnitude           = Signal(32)
        p_magnitude_stage     = Signal(32)
        i_product             = Signal(64)
        i_product_high        = Signal(32)
        i_product_low         = Signal(32)
        i_over_limit          = Signal()
        i_magnitude           = Signal(32)
        i_magnitude_stage     = Signal(32)
        rate_integral         = Signal((33, True))
        p_magnitude_signed    = Signal((34, True))
        i_magnitude_signed    = Signal((34, True))
        signed_p_delta        = Signal((34, True))
        signed_i_delta        = Signal((34, True))
        integral_sum          = Signal((34, True))
        integral_clamped      = Signal((34, True))
        command_sum           = Signal((34, True))
        command_clamped       = Signal((34, True))
        rate_limit_stage0     = Signal(32)
        rate_limit_stage1     = Signal(32)
        rate_limit_signed     = Signal((34, True))
        rate_limit_negative   = Signal((34, True))
        rate_negative_stage0  = Signal()
        rate_negative_stage1  = Signal()
        rate_invert_stage0    = Signal()
        rate_invert_stage1    = Signal()
        rate_update_stage0    = Signal()
        rate_update_stage1    = Signal()
        rate_holdover_stage0  = Signal()
        rate_holdover_stage1  = Signal()
        rate_limited_stage    = Signal()
        effective_half_period   = Signal(32)
        effective_lock_window   = Signal(32)
        reference_live          = Signal()
        holdover_allowed        = Signal()
        reference_pulse_d       = Signal()
        reference_rising        = Signal()
        reference_tick          = Signal()
        reference_spacing_ticks = Signal(32)

        self.comb += [
            reference_live.eq(reference_valid),
            self.reference_locked.eq(reference_live),
            holdover_allowed.eq(self.holdover_enable & ever_locked),
            self.holdover.eq(self.enable & ~reference_live & holdover_allowed),
            self.active.eq(self.enable & (reference_live | self.holdover)),
            self.mmcm_override.eq(self.enable),
            self.mmcm_enable.eq(self.active),
            self.mmcm_rate.eq(self.last_rate),
            self.mmcm_update_cycles.eq(self.update_cycles_cfg),
            self.aligned.eq(aligned_once),
            If(self.half_period_ticks_cfg == 0,
                effective_half_period.eq(1),
            ).Else(
                effective_half_period.eq(self.half_period_ticks_cfg),
            ),
            If(self.lock_window_ticks_cfg == 0,
                effective_lock_window.eq(1),
            ).Else(
                effective_lock_window.eq(self.lock_window_ticks_cfg),
            ),
            If(sample_error_ticks < 0,
                sample_abs_ticks.eq(-sample_error_ticks),
            ).Else(
                sample_abs_ticks.eq(sample_error_ticks),
            ),
            sample_in_lock_window.eq(sample_abs_ticks <= effective_lock_window),
            p_product_high.eq(p_product[32:64]),
            p_product_low.eq(p_product[0:32]),
            p_over_limit.eq((p_product_high != 0) | (p_product_low > rate_limit_stage0)),
            p_magnitude.eq(Mux(p_over_limit, rate_limit_stage0, p_product_low)),
            i_product_high.eq(i_product[32:64]),
            i_product_low.eq(i_product[0:32]),
            i_over_limit.eq((i_product_high != 0) | (i_product_low > rate_limit_stage0)),
            i_magnitude.eq(Mux(i_over_limit, rate_limit_stage0, i_product_low)),
            p_magnitude_signed.eq(p_magnitude_stage),
            i_magnitude_signed.eq(i_magnitude_stage),
            If(rate_negative_stage1 ^ rate_invert_stage1,
                signed_p_delta.eq(-p_magnitude_signed),
                signed_i_delta.eq(-i_magnitude_signed),
            ).Else(
                signed_p_delta.eq(p_magnitude_signed),
                signed_i_delta.eq(i_magnitude_signed),
            ),
            integral_sum.eq(rate_integral + signed_i_delta),
            rate_limit_signed.eq(rate_limit_stage1),
            rate_limit_negative.eq(-rate_limit_signed),
            If(integral_sum > rate_limit_signed,
                integral_clamped.eq(rate_limit_signed),
            ).Elif(integral_sum < rate_limit_negative,
                integral_clamped.eq(rate_limit_negative),
            ).Else(
                integral_clamped.eq(integral_sum),
            ),
            command_sum.eq(integral_clamped + signed_p_delta),
            If(command_sum > rate_limit_signed,
                command_clamped.eq(rate_limit_signed),
            ).Elif(command_sum < rate_limit_negative,
                command_clamped.eq(rate_limit_negative),
            ).Else(
                command_clamped.eq(command_sum),
            ),
            reference_rising.eq(reference_pulse & ~reference_pulse_d),
            reference_tick.eq(reference_rising & (reference_spacing_ticks >= effective_half_period)),
        ]

        # Phase Detector / Rate Pipeline.
        # -------------------------------
        # Pair each PTP reference edge with the nearest clk10 marker inside the search
        # window, then pipeline the PI rate calculation to keep the sys_clk path short.
        self.sync += [
            align_clk10.eq(0),
            sample_valid.eq(0),
            reference_pulse_d.eq(reference_pulse),

            If(ticks_since_ref < effective_half_period,
                ticks_since_ref.eq(ticks_since_ref + 1),
            ),
            If(ticks_since_clk10 < effective_half_period,
                ticks_since_clk10.eq(ticks_since_clk10 + 1),
            ),
            If(reference_spacing_ticks < effective_half_period,
                reference_spacing_ticks.eq(reference_spacing_ticks + 1),
            ),
            If(reference_rising,
                reference_spacing_ticks.eq(0),
            ),

            If(self.clear_counters,
                self.sample_count.eq(0),
                self.reference_count.eq(0),
                self.clk10_count.eq(0),
                self.missing_count.eq(0),
                self.align_count.eq(0),
                self.lock_loss_count.eq(0),
                self.rate_update_count.eq(0),
                self.saturation_count.eq(0),
            ),

            If(self.manual_align,
                # Software requests are intentionally phase-qualified by the
                # next PTP reference edge.  Resetting the clk10 marker at CSR
                # write time would make the RF reference phase depend on host
                # command latency rather than on the disciplined board time.
                align_pending.eq(1),
                self.waiting_after_ref.eq(0),
            ),

            If(~self.enable,
                aligned_once.eq(0),
                ever_locked.eq(0),
                prev_locked.eq(0),
                self.locked.eq(0),
                self.last_rate.eq(0),
                rate_integral.eq(0),
                self.rate_limited.eq(0),
                align_pending.eq(0),
                rate_multiply_pending.eq(0),
                rate_limit_pending.eq(0),
                rate_apply_pending.eq(0),
            ).Elif(~reference_live & ~self.holdover,
                aligned_once.eq(0),
                self.locked.eq(0),
                self.last_rate.eq(0),
                rate_integral.eq(0),
                self.rate_limited.eq(0),
                align_pending.eq(0),
                rate_multiply_pending.eq(0),
                rate_limit_pending.eq(0),
                rate_apply_pending.eq(0),
            ),

            If(reference_tick,
                self.reference_count.eq(self.reference_count + 1),
                ticks_since_ref.eq(0),
            ),
            If(self.clk10_ps.o,
                self.clk10_count.eq(self.clk10_count + 1),
                ticks_since_clk10.eq(0),
                have_clk10.eq(1),
            ),

            # Error sign convention: a clk10 marker before the PTP reference is negative;
            # a marker after the reference is positive.
            If(self.enable & reference_live & reference_tick & (~aligned_once | align_pending),
                align_clk10.eq(1),
                aligned_once.eq(1),
                align_pending.eq(0),
                self.align_count.eq(self.align_count + 1),
                self.waiting_after_ref.eq(0),
            ).Elif(reference_tick & self.clk10_ps.o,
                sample_valid.eq(1),
                sample_error_ticks.eq(0),
                self.waiting_after_ref.eq(0),
            ).Elif(reference_tick,
                If(have_clk10 & (ticks_since_clk10 < effective_half_period),
                    sample_valid.eq(1),
                    sample_error_ticks.eq(-ticks_since_clk10),
                    self.waiting_after_ref.eq(0),
                ).Else(
                    self.waiting_after_ref.eq(1),
                )
            ).Elif(self.clk10_ps.o & self.waiting_after_ref,
                sample_valid.eq(1),
                sample_error_ticks.eq(ticks_since_ref),
                self.waiting_after_ref.eq(0),
            ).Elif(self.waiting_after_ref & (ticks_since_ref >= effective_half_period),
                self.waiting_after_ref.eq(0),
                self.missing_count.eq(self.missing_count + 1),
            ),

            If(sample_valid,
                self.sample_count.eq(self.sample_count + 1),
                self.last_error_ticks.eq(sample_error_ticks),
                self.last_error_ns.eq(sample_error_ticks * sys_clk_period_ns),
                If(sample_in_lock_window,
                    self.locked.eq(reference_live),
                    ever_locked.eq(ever_locked | reference_live),
                ).Else(
                    self.locked.eq(0),
                ),
                If(prev_locked & ~sample_in_lock_window,
                    self.lock_loss_count.eq(self.lock_loss_count + 1),
                ),
                prev_locked.eq(reference_live & sample_in_lock_window),

                If(self.enable,
                    rate_error_abs.eq(sample_abs_ticks),
                    rate_negative_base.eq(sample_error_ticks > 0),
                    rate_multiply_pending.eq(1),
                )
            ),

            # PI stage 0: multiply the absolute phase error by configured gains.
            If(rate_multiply_pending,
                rate_multiply_pending.eq(0),
                p_product.eq(rate_error_abs * self.p_gain_cfg),
                i_product.eq(rate_error_abs * self.i_gain_cfg),
                rate_limit_stage0.eq(self.rate_limit_cfg),
                rate_negative_stage0.eq(rate_negative_base),
                rate_invert_stage0.eq(self.invert),
                rate_update_stage0.eq(self.enable & reference_live),
                rate_holdover_stage0.eq(self.holdover),
                rate_limit_pending.eq(1),
            ),

            # PI stage 1: clamp proportional/integral magnitudes to the rate limit.
            If(rate_limit_pending,
                rate_limit_pending.eq(0),
                p_magnitude_stage.eq(p_magnitude),
                i_magnitude_stage.eq(i_magnitude),
                rate_limit_stage1.eq(rate_limit_stage0),
                rate_limited_stage.eq(p_over_limit | i_over_limit),
                rate_negative_stage1.eq(rate_negative_stage0),
                rate_invert_stage1.eq(rate_invert_stage0),
                rate_update_stage1.eq(rate_update_stage0),
                rate_holdover_stage1.eq(rate_holdover_stage0),
                rate_apply_pending.eq(1),
            ),

            # PI stage 2: apply signed P/I terms or hold the last rate in holdover.
            If(rate_apply_pending,
                rate_apply_pending.eq(0),
                self.rate_limited.eq(rate_limited_stage),
                If(rate_limited_stage,
                    self.saturation_count.eq(self.saturation_count + 1),
                ),
                If(rate_update_stage1,
                    self.rate_update_count.eq(self.rate_update_count + 1),
                    rate_integral.eq(integral_clamped),
                    self.last_rate.eq(command_clamped),
                ).Elif(~rate_holdover_stage1,
                    self.last_rate.eq(0),
                    rate_integral.eq(0),
                )
            ),
        ]

        if with_csr:
            self.add_csr(sys_clk_period_ns)

    def add_csr(self, sys_clk_period_ns):
        self._control = CSRStorage(fields=[
            CSRField("enable",         size=1, offset=0, reset=0, values=[
                ("``0b0``", "PTP-to-10MHz clock discipline disabled."),
                ("``0b1``", "Allow PTP-to-10MHz clock discipline to override the clk10 MMCM phase driver."),
            ]),
            CSRField("holdover",       size=1, offset=1, reset=1, values=[
                ("``0b0``", "Disable MMCM steering when the PTP time reference is not locked."),
                ("``0b1``", "Keep the last MMCM phase-step rate during PTP reference loss."),
            ]),
            CSRField("invert",         size=1, offset=2, reset=0,
                description="Invert the MMCM phase-step sign used by the PTP-to-10MHz loop."),
            CSRField("align",          size=1, offset=8, pulse=True,
                description="Reset the clk10 1Hz phase marker on the next PTP reference pulse."),
            CSRField("clear_counters", size=1, offset=9, pulse=True,
                description="Clear PTP-to-10MHz monitor counters."),
        ])
        self._update_cycles = CSRStorage(32,
            reset=self.update_cycles_cfg.reset.value,
            description="MMCMPhaseDiscipline update interval used while the PTP-to-10MHz loop owns clk10."
        )
        self._p_gain = CSRStorage(32,
            reset=self.p_gain_cfg.reset.value,
            description="Proportional gain in Q0.32 MMCM fine steps per update per sys-clock tick of phase error."
        )
        self._i_gain = CSRStorage(32,
            reset=self.i_gain_cfg.reset.value,
            description="Integral gain in Q0.32 MMCM fine steps per update per sys-clock tick of phase error."
        )
        self._rate_limit = CSRStorage(32,
            reset=self.rate_limit_cfg.reset.value,
            description="Absolute MMCM fine phase-step rate limit in Q0.32 steps per update."
        )
        self._lock_window_ticks = CSRStorage(32,
            reset=self.lock_window_ticks_cfg.reset.value,
            description="PTP-to-10MHz lock window in sys-clock ticks."
        )
        self._half_period_ticks = CSRStorage(32,
            reset=self.half_period_ticks_cfg.reset.value,
            description="Maximum phase-detector search window in sys-clock ticks."
        )
        self._status = CSRStatus(fields=[
            CSRField("enable",            size=1, offset=0,
                description="PTP-to-10MHz clock discipline enabled."),
            CSRField("active",            size=1, offset=1,
                description="PTP-to-10MHz loop currently owns the clk10 MMCM phase driver."),
            CSRField("reference_locked", size=1, offset=2,
                description="PTP board-time reference is locked."),
            CSRField("clock_locked",      size=1, offset=3,
                description="clk10 1Hz marker is within the configured lock window."),
            CSRField("holdover",          size=1, offset=4,
                description="PTP reference was lost; keeping the last phase-step rate."),
            CSRField("aligned",           size=1, offset=5,
                description="clk10 1Hz marker has been aligned since the loop was enabled."),
            CSRField("waiting_after_ref", size=1, offset=6,
                description="Phase detector is waiting for a clk10 marker after the PTP reference pulse."),
            CSRField("rate_limited",      size=1, offset=7,
                description="Most recent rate calculation hit the configured rate limit."),
        ])
        self._phase_tick_ps = CSRStatus(32,
            description="Phase detector tick period in picoseconds."
        )
        self._last_error_ticks = CSRStatus(32,
            description="Last signed clk10-marker minus PTP-reference phase error in sys-clock ticks."
        )
        self._last_error_ns = CSRStatus(64,
            description="Last signed clk10-marker minus PTP-reference phase error in nanoseconds."
        )
        self._last_rate = CSRStatus(32,
            description="Last MMCM fine phase-step rate command, signed Q0.32 steps per update."
        )
        self._sample_count = CSRStatus(32,
            description="Number of valid PTP-to-10MHz phase samples."
        )
        self._reference_count = CSRStatus(32,
            description="Number of PTP reference pulses observed."
        )
        self._clk10_count = CSRStatus(32,
            description="Number of clk10 1Hz marker pulses observed."
        )
        self._missing_count = CSRStatus(32,
            description="Number of phase-detector windows that expired before the matching marker arrived."
        )
        self._align_count = CSRStatus(32,
            description="Number of clk10 marker alignments requested."
        )
        self._lock_loss_count = CSRStatus(32,
            description="Number of clk10 marker lock-loss events."
        )
        self._rate_update_count = CSRStatus(32,
            description="Number of MMCM phase-step rate updates produced by the PTP-to-10MHz loop."
        )
        self._saturation_count = CSRStatus(32,
            description="Number of rate calculations limited by rate_limit."
        )

        self.comb += [
            self.enable.eq(self._control.fields.enable),
            self.holdover_enable.eq(self._control.fields.holdover),
            self.invert.eq(self._control.fields.invert),
            self.manual_align.eq(self._control.fields.align),
            self.clear_counters.eq(self._control.fields.clear_counters),
            self.update_cycles_cfg.eq(self._update_cycles.storage),
            self.p_gain_cfg.eq(self._p_gain.storage),
            self.i_gain_cfg.eq(self._i_gain.storage),
            self.rate_limit_cfg.eq(self._rate_limit.storage),
            self.lock_window_ticks_cfg.eq(self._lock_window_ticks.storage),
            self.half_period_ticks_cfg.eq(self._half_period_ticks.storage),
            self._status.fields.enable.eq(self.enable),
            self._status.fields.active.eq(self.active),
            self._status.fields.reference_locked.eq(self.reference_locked),
            self._status.fields.clock_locked.eq(self.locked),
            self._status.fields.holdover.eq(self.holdover),
            self._status.fields.aligned.eq(self.aligned),
            self._status.fields.waiting_after_ref.eq(self.waiting_after_ref),
            self._status.fields.rate_limited.eq(self.rate_limited),
            self._phase_tick_ps.status.eq(int(sys_clk_period_ns * 1000)),
            self._last_error_ticks.status.eq(self.last_error_ticks[0:32]),
            self._last_error_ns.status.eq(self.last_error_ns[0:64]),
            self._last_rate.status.eq(self.last_rate[0:32]),
            self._sample_count.status.eq(self.sample_count),
            self._reference_count.status.eq(self.reference_count),
            self._clk10_count.status.eq(self.clk10_count),
            self._missing_count.status.eq(self.missing_count),
            self._align_count.status.eq(self.align_count),
            self._lock_loss_count.status.eq(self.lock_loss_count),
            self._rate_update_count.status.eq(self.rate_update_count),
            self._saturation_count.status.eq(self.saturation_count),
        ]
