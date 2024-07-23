#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litepcie.common import *

from litex.soc.interconnect.csr import *

from litex.soc.cores.pwm     import PWM
from litex.soc.cores.bitbang import I2CMaster

# SI5351 -------------------------------------------------------------------------------------------

class SI5351(LiteXModule):
    def __init__(self, platform):
        # I2C Master.
        self.i2c = I2CMaster(pads=platform.request("si5351_i2c"))

        # VCXO PWM.
        self.pwm = PWM(platform.request("si5351_pwm"),
            default_enable = 1,
            default_width  = 1024,
            default_period = 2048,
        )

        # Enable / Clkin.
        self.comb += platform.request("si5351_en_clkin").eq(1)
