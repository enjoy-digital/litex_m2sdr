#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.fhdl.specials import Tristate

from litex.gen import *

from litex.build.io import DDROutput

from litepcie.common import *

from litex.soc.interconnect.csr import *

from litex.soc.cores.pwm     import PWM
from litex.soc.cores.bitbang import I2CMaster

from litex_m2sdr.gateware.si5351_i2c import SI5351I2C, i2c_program_si5351

# SI5351 -------------------------------------------------------------------------------------------

class SI5351(LiteXModule):
    def __init__(self, platform, sys_clk_freq, clk_in=0, with_csr=True):
        self.version    = Signal() # SI5351 Version (0=B, 1=C).
        self.ss_en      = Signal()   # SI5351 Spread spectrum enable (versions A and B).
        self.clk_in_src = Signal() # SI5351 ClkIn Source.

        # # #

        # I2C Pads.
        i2c_pads = platform.request("si5351_i2c")

        # I2C Master.
        fake_i2c_pads = Record([("scl", 1), ("sda", 1)])
        self.i2c = I2CMaster(pads=fake_i2c_pads, connect_pads=False)

        # I2C Gateware Init.
        fake_i2c_init_pads = Record([("scl_o", 1), ("sda_o", 1), ("sda_i", 1)])
        self.i2c_init = SI5351I2C(
            pads         = fake_i2c_init_pads,
            program      = i2c_program_si5351,
            sys_clk_freq = sys_clk_freq,
        )

        # I2C Muxing.
        i2c_scl_o = Signal()
        i2c_sda_o = Signal()
        i2c_sda_i = Signal()
        self.sync += [
            # Give access to I2C Master when Gateware sequencer is done.
            If(self.i2c_init.sequencer.done,
                i2c_scl_o.eq(self.i2c._w.fields.scl),
                i2c_sda_o.eq(~(self.i2c._w.fields.oe & ~self.i2c._w.fields.sda)),
                self.i2c._r.fields.sda.eq(i2c_sda_i),
            # Else give access to Gateware sequencer.
            ).Else(
                i2c_scl_o.eq(fake_i2c_init_pads.scl_o),
                i2c_sda_o.eq(fake_i2c_init_pads.sda_o),
                fake_i2c_init_pads.sda_i.eq(i2c_sda_i),
            )
        ]

        # I2C Tristates.
        self.specials += Tristate(i2c_pads.scl,
            o  = 0,         # I2C uses Pull-ups, only drive low.
            oe = ~i2c_scl_o # Drive when scl is low.
        )
        self.specials += Tristate(i2c_pads.sda,
            o  = 0,          # I2C uses Pull-ups, only drive low.
            oe = ~i2c_sda_o, # Drive when oe and sda is low.
            i  = i2c_sda_i
        )

        # VCXO PWM.
        self.pwm = PWM(platform.request("si5351_pwm"),
            default_enable = 1,
            default_width  = 1024,
            default_period = 2048,
        )

        # Enable / Clkin.
        platform.add_platform_command("set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk10_clk]")
        self.specials += Instance("BUFGMUX",
            i_S  = self.version,
            i_I0 = self.ss_en,
            i_I1 = ClockSignal("clk10"),
            o_O  = platform.request("si5351_ssen_clkin"),
        )

        # CSRs.
        if with_csr:
            self.add_csr()

    def add_csr(self, default_version=0, default_clk_in=0):
        self.control = CSRStorage(fields=[
            CSRField("version",  size=1, offset=0, values=[
                ("``0b0``", "SI5351B Version."),
                ("``0b1``", "SI5351C Version."),
            ], reset=default_version),
            CSRField("clk_in_src",  size=1, offset=1, values=[ # FIXME use this instead of version
                ("``0b0``", "10MHz ClkIn from XO."),
                ("``0b1``", "10MHz ClkIn from FPGA."),
            ], reset=default_clk_in),
            CSRField("ss_en",  size=1, offset=2, values=[
                ("``0b0``", "SI5351B Spread spectrum disabled."),
                ("``0b1``", "SI5351B Spread spectrum enabled."),
            ], reset=default_version),
        ])

        # # #

        self.comb += [
            self.version.eq(self.control.fields.version),
            self.clk_in_src.eq(self.control.fields.clk_in_src),
        ]
