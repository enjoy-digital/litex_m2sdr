#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.cores.pwm import PWM

# Helpers ------------------------------------------------------------------------------------------

def gray_encode(sig):
    return sig ^ (sig >> 1)

# PI Regulator -------------------------------------------------------------------------------------

class PIRegulator(LiteXModule):
    def __init__(self, error, reset, update, control_bits=32, frac_bits=20, default_kp=1, default_ki=1):
        # Inputs.
        self.error  = error
        self.reset  = reset
        self.update = update
        self.kp     = Signal(32, reset=default_kp << frac_bits)
        self.ki     = Signal(32, reset=default_ki << frac_bits)

        # Outputs.
        self.control    = Signal((control_bits, True))
        self.integrator = Signal((control_bits, True))

        # # #

        _control      = Signal((control_bits + frac_bits, True))
        _proportional = Signal((control_bits + frac_bits, True))
        _integral     = Signal((control_bits + frac_bits, True))

        self.sync += [
            If(self.update,
                self.integrator.eq(self.integrator + self.error)
            ),
            If(self.reset,
                self.integrator.eq(0)
            ),
        ]

        self.sync += [
            _proportional.eq(self.error      * self.kp),
            _integral.eq(    self.integrator * self.ki),
            If(self.update,
                _control.eq(_control + _proportional + _integral),
            ),
            If(self.reset,
                _control.eq(0)
            ),
            self.control.eq(_control[frac_bits:])
        ]

# Duty Bias ----------------------------------------------------------------------------------------

class DutyBias(LiteXModule):
    def __init__(self, duty_in, duty_bias=None, duty_bits=11):
        self.duty_out = Signal((len(duty_in) + 1, True))

        # # #

        if duty_bias is None:
            duty_bias = 1 << (duty_bits - 1)

        self.comb += self.duty_out.eq(duty_in + duty_bias)

# Duty Clamper -------------------------------------------------------------------------------------

class DutyClamper(LiteXModule):
    def __init__(self, duty_in, duty_bits=11):
        self.duty_out = Signal(duty_bits)

        # # #

        min_duty = 0
        max_duty = (1 << duty_bits) - 1

        self.comb += [
            If(duty_in < 0,
                self.duty_out.eq(0)
            ).Elif(duty_in > max_duty,
                self.duty_out.eq(max_duty)
            ).Else(
                self.duty_out.eq(duty_in)
            )
        ]

# VCXO Ref Discipliner ----------------------------------------------------------------------------

class VCXORefDiscipliner(LiteXModule):
    def __init__(self, ref_clk, vcxo_clk, sys_clk_freq, vcxo_cycles_per_ref=10,
        duty_bits=11, frac_bits=20, default_kp=1, default_ki=1,
        default_update_shift=13, default_phase_avg_shift=4,
        default_timeout=0.2):
        # IOs.
        self.enable      = Signal()
        self.mode        = Signal()
        self.ref_present = Signal()
        self.error       = Signal((32, True))
        self.duty        = Signal(duty_bits)

        # # #

        # Parameters.
        # -----------
        timeout_cycles = int(sys_clk_freq * default_timeout)
        assert timeout_cycles < (1 << 32)

        # Ref CDC.
        # --------
        ref_sync = Signal()
        ref_d    = Signal()
        self.specials += MultiReg(ref_clk, ref_sync)
        self.sync += ref_d.eq(ref_sync)
        ref_rise = Signal()
        self.comb += ref_rise.eq(ref_sync & ~ref_d)

        # VCXO Clock Domain.
        # ------------------
        self.cd_vcxo = ClockDomain()
        self.comb += self.cd_vcxo.clk.eq(vcxo_clk)

        vcxo_count     = Signal(32)
        vcxo_gray      = Signal(32)
        vcxo_gray_sync = Signal(32)
        vcxo_count_sys = Signal(32)
        vcxo_mod_bits  = max(1, bits_for(vcxo_cycles_per_ref - 1))
        vcxo_mod       = Signal(vcxo_mod_bits)
        vcxo_mod_sync  = Signal(vcxo_mod_bits)

        self.sync.vcxo += vcxo_count.eq(vcxo_count + 1)
        self.sync.vcxo += vcxo_gray.eq(gray_encode(vcxo_count))
        self.sync.vcxo += If(vcxo_mod == (vcxo_cycles_per_ref - 1),
            vcxo_mod.eq(0)
        ).Else(
            vcxo_mod.eq(vcxo_mod + 1)
        )
        self.specials += MultiReg(vcxo_gray, vcxo_gray_sync)
        self.specials += MultiReg(vcxo_mod, vcxo_mod_sync)

        # Gray to binary.
        gray_to_bin = Signal(32)
        for i in range(32):
            expr = vcxo_gray_sync[i]
            for j in range(i + 1, 32):
                expr = expr ^ vcxo_gray_sync[j]
            self.comb += gray_to_bin[i].eq(expr)
        self.comb += vcxo_count_sys.eq(gray_to_bin)

        # Ref edge processing.
        # --------------------
        vcxo_count_prev = Signal(32)
        vcxo_delta_next = Signal(32)
        error_next      = Signal((32, True))
        self.error_limit = error_limit = Signal((32, True), reset=vcxo_cycles_per_ref)

        self.comb += [
            vcxo_delta_next.eq(vcxo_count_sys - vcxo_count_prev),
            error_next.eq(vcxo_cycles_per_ref - vcxo_delta_next),
        ]

        self.sync += If(ref_rise, vcxo_count_prev.eq(vcxo_count_sys))

        # Phase detector (in VCXO cycles).
        # --------------------------------
        phase_raw    = Signal(32)
        self.phase_target = phase_target = Signal(32, reset=0)
        phase_diff   = Signal((32, True))
        phase_error  = Signal((32, True))
        phase_half   = vcxo_cycles_per_ref // 2

        self.comb += [
            phase_raw.eq(vcxo_mod_sync),
            phase_diff.eq(phase_raw - phase_target),
        ]
        self.comb += [
            If(phase_diff > phase_half,
                phase_error.eq(phase_diff - vcxo_cycles_per_ref)
            ).Elif(phase_diff < -phase_half,
                phase_error.eq(phase_diff + vcxo_cycles_per_ref)
            ).Else(
                phase_error.eq(phase_diff)
            )
        ]

        # Ref present watchdog.
        # ---------------------
        ref_timeout = Signal(32)
        self.sync += [
            If(~self.enable,
                ref_timeout.eq(0),
                self.ref_present.eq(0),
            ).Elif(ref_rise,
                ref_timeout.eq(0),
                self.ref_present.eq(1),
            ).Else(
                If(ref_timeout >= timeout_cycles,
                    self.ref_present.eq(0)
                ).Else(
                    ref_timeout.eq(ref_timeout + 1)
                )
            )
        ]

        # Error accumulation / decimation.
        # --------------------------------
        self.update_shift    = update_shift    = Signal(4, reset=default_update_shift)
        self.phase_avg_shift = phase_avg_shift = Signal(4, reset=default_phase_avg_shift)
        update_count   = Signal(16)
        update_fire    = Signal()
        effective_shift = Signal(4)
        err_acc        = Signal((32, True))
        err_acc_next   = Signal((32, True))
        error_clamped  = Signal((32, True))
        meas_error     = Signal((32, True))

        self.comb += [
            effective_shift.eq(Mux(self.mode, phase_avg_shift, update_shift)),
            update_fire.eq(update_count == ((1 << effective_shift) - 1)),
            meas_error.eq(Mux(self.mode, phase_error, error_next)),
            If(meas_error > error_limit,
                error_clamped.eq(error_limit)
            ).Elif(meas_error < -error_limit,
                error_clamped.eq(-error_limit)
            ).Else(
                error_clamped.eq(meas_error)
            ),
            err_acc_next.eq(err_acc + error_clamped),
        ]

        self.sync += [
            If(~self.enable,
                update_count.eq(0),
                err_acc.eq(0),
                self.error.eq(0),
            ).Elif(ref_rise & self.ref_present,
                self.error.eq(error_clamped),
                If(update_fire,
                    err_acc.eq(0),
                    update_count.eq(0),
                ).Else(
                    err_acc.eq(err_acc_next),
                    update_count.eq(update_count + 1)
                )
            )
        ]

        # PI Regulator.
        # -------------
        pi_update = Signal()
        self.comb += pi_update.eq(ref_rise & self.ref_present & update_fire)
        pi_error = Signal((32, True))
        self.comb += pi_error.eq(err_acc_next >> effective_shift)
        self.pi = PIRegulator(
            error        = pi_error,
            reset        = ~self.enable,
            update       = pi_update,
            control_bits = 32,
            frac_bits    = frac_bits,
            default_kp   = default_kp,
            default_ki   = default_ki,
        )

        # Duty Bias/Clamp.
        # ----------------
        self.bias = Signal(duty_bits, reset=1 << (duty_bits - 1))
        self.duty_bias = DutyBias(
            duty_in   = self.pi.control,
            duty_bias = self.bias,
            duty_bits = duty_bits,
        )
        self.duty_clamp = DutyClamper(
            duty_in   = self.duty_bias.duty_out,
            duty_bits = duty_bits,
        )
        self.comb += self.duty.eq(self.duty_clamp.duty_out)

        # PWM Output (no CSR).
        # --------------------
        self.pwm = Signal()
        self.pwm_gen = PWM(
            pwm            = self.pwm,
            default_enable = 1,
            default_width  = 0,
            default_period = (1 << duty_bits) - 1,
            with_csr       = False,
        )
        self.comb += self.pwm_gen.width.eq(self.duty)

        # CSR.
        # ----
        self.add_csr()

    def add_csr(self):
        self.control = CSRStorage(fields=[
            CSRField("enable", offset=0, size=1, reset=0, description="VCXO discipliner enable."),
            CSRField("mode",   offset=1, size=1, reset=0, description="0: freq lock, 1: phase lock."),
        ])
        self._bias         = CSRStorage(len(self.duty), reset=self.bias.reset, description="Duty bias (mid-scale default).")
        self._kp           = CSRStorage(32, reset=self.pi.kp.reset, description="Proportional gain.")
        self._ki           = CSRStorage(32, reset=self.pi.ki.reset, description="Integral gain.")
        self._update_shift = CSRStorage(4,  reset=self.update_shift.reset, description="Update decimation (2^N ref edges).")
        self._phase_avg_shift = CSRStorage(4, reset=self.phase_avg_shift.reset, description="Phase averaging (2^N ref edges).")
        self._phase_target = CSRStorage(32, reset=0, description="Phase target (VCXO cycles within ref period).")
        self._error_limit  = CSRStorage(32, reset=self.error_limit.reset, description="Error clamp (cycles).")

        self._error        = CSRStatus(32, description="Current error (signed).")
        self._ref_present  = CSRStatus(1,  description="Reference present.")
        self._duty         = CSRStatus(len(self.duty), description="Duty value.")

        self.comb += [
            self.enable.eq(self.control.fields.enable),
            self.mode.eq(self.control.fields.mode),
            self.bias.eq(self._bias.storage),
            self.pi.kp.eq(self._kp.storage),
            self.pi.ki.eq(self._ki.storage),
            self.update_shift.eq(self._update_shift.storage),
            self.phase_avg_shift.eq(self._phase_avg_shift.storage),
            self.phase_target.eq(self._phase_target.storage),
            self.error_limit.eq(self._error_limit.storage),

            self._error.status.eq(self.error),
            self._ref_present.status.eq(self.ref_present),
            self._duty.status.eq(self.duty),
        ]
