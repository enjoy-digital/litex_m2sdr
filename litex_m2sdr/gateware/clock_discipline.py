#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *

# MMCM Phase Discipline -----------------------------------------------------------------------------

class MMCMPhaseDiscipline(LiteXModule):
    """Drive a MMCM Dynamic Phase Shift port with bounded phase-step requests."""
    def __init__(self, sys_clk_freq, with_csr=True):
        # Config.
        # -------
        self.enable             = Signal()
        self.update_cycles_cfg  = Signal(32, reset=max(1, int(sys_clk_freq // 1_000_000)))
        self.rate_cfg           = Signal((32, True))
        self.manual_inc         = Signal()
        self.manual_dec         = Signal()
        self.clear_counters     = Signal()

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
        effective_update_cycles = Signal(32)
        rate_negative           = Signal()
        rate_abs                = Signal(32)
        accumulator_sum         = Signal(33)
        auto_step               = Signal()
        step_inc_request        = Signal()
        step_dec_request        = Signal()

        self.comb += [
            If(self.update_cycles_cfg == 0,
                effective_update_cycles.eq(1),
            ).Else(
                effective_update_cycles.eq(self.update_cycles_cfg),
            ),
            rate_negative.eq(self.rate_cfg < 0),
            If(self.rate_cfg < 0,
                rate_abs.eq(-self.rate_cfg),
            ).Else(
                rate_abs.eq(self.rate_cfg),
            ),
            accumulator_sum.eq(accumulator + rate_abs),
            auto_step.eq(self.enable & sample_tick & accumulator_sum[32]),
            step_inc_request.eq(self.manual_inc | (auto_step & ~rate_negative)),
            step_dec_request.eq(self.manual_dec | (auto_step &  rate_negative)),
            self.pending.eq(step_inc_request | step_dec_request),
        ]

        # Sampling / Accumulator.
        # -----------------------
        self.sync += [
            self.psen.eq(0),
            If(sample_counter >= (effective_update_cycles - 1),
                sample_counter.eq(0),
                sample_tick.eq(1),
            ).Else(
                sample_counter.eq(sample_counter + 1),
                sample_tick.eq(0),
            ),
            If(sample_tick,
                If(self.enable,
                    accumulator.eq(accumulator_sum[:32]),
                ).Else(
                    accumulator.eq(0),
                )
            )
        ]

        # MMCM DPS Requests / Counters.
        # -----------------------------
        self.sync += [
            If(self.clear_counters,
                self.step_count.eq(0),
                self.inc_count.eq(0),
                self.dec_count.eq(0),
                self.dropped_count.eq(0),
            ),
            If(self.busy,
                If(self.psdone,
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
                    self.psen.eq(1),
                    self.psincdec.eq(1),
                    self.last_direction.eq(1),
                    self.busy.eq(1),
                ).Elif(step_dec_request,
                    self.psen.eq(1),
                    self.psincdec.eq(0),
                    self.last_direction.eq(0),
                    self.busy.eq(1),
                )
            )
        ]

        if with_csr:
            self.add_csr()

    def add_csr(self):
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "MMCM fine phase discipline disabled."),
                ("``0b1``", "MMCM fine phase discipline enabled."),
            ]),
            CSRField("manual_inc", size=1, offset=1, pulse=True,
                description="Request one positive MMCM fine phase step."),
            CSRField("manual_dec", size=1, offset=2, pulse=True,
                description="Request one negative MMCM fine phase step."),
            CSRField("clear_counters", size=1, offset=8, pulse=True,
                description="Clear MMCM phase discipline counters."),
        ])
        self._rate = CSRStorage(32,
            description="Signed MMCM fine phase-step rate (two's complement, Q0.32 steps per update interval)."
        )
        self._update_cycles = CSRStorage(32,
            reset       = self.update_cycles_cfg.reset,
            description = "Number of sys_clk cycles between MMCM fine phase discipline updates."
        )
        self._status = CSRStatus(fields=[
            CSRField("busy", size=1, offset=0,
                description="MMCM fine phase shift request in progress."),
            CSRField("pending", size=1, offset=1,
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
