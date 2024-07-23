#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.build.io import DDROutput

from litepcie.common import *

from litex.soc.interconnect.csr import *

from litex.soc.cores.pwm     import PWM
from litex.soc.cores.bitbang import I2CMaster

# SI5351 -------------------------------------------------------------------------------------------

class SI5351(LiteXModule):
    def __init__(self, platform, with_csr=True):
        self.version = Signal() # SI5351 Version (0=B, 1=C).

        # # #

        # I2C Master.
        self.i2c = I2CMaster(pads=platform.request("si5351_i2c"))

        # VCXO PWM.
        self.pwm = PWM(platform.request("si5351_pwm"),
            default_enable = 1,
            default_width  = 1024,
            default_period = 2048,
        )

        # Enable / Clkin.
        si5351_en_clkin = Signal()
        self.specials += DDROutput(
            i1  = 1,
            i2  = ~self.version, # B = Enable, always 1. C = Generate 10MHz.
            o   = platform.request("si5351_en_clkin"),
            clk = ClockSignal("clk10")
        )

        # CSRs.
        if with_csr:
            self.add_csr()

    def add_csr(self, default_version=0):
        self.control = CSRStorage(fields=[
            CSRField("version",  size=1, offset=0, values=[
                ("``0b0``", "SI5351B."),
                ("``0b1``", "SI5351C."),
            ], reset=default_version)
        ])

        # # #

        self.comb += self.version.eq(self.control.fields.version)
