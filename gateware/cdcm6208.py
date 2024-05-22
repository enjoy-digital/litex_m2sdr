#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.cores.spi import SPIMaster

# CDCM 6208 ----------------------------------------------------------------------------------------

class CDCM6208(LiteXModule):
    def __init__(self, pads, sys_clk_freq):
        self.pads    = pads
        self._reset  = CSRStorage()
        self._status = CSRStatus(2)

        # # #

        self.sync += pads.rst_n.eq(~self._reset.storage)
        self.specials += MultiReg(pads.status, self._status.status)

        self.spi = SPIMaster(pads, 32, sys_clk_freq, 5e6)
