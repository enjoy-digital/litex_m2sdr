#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer


# PCIe Link Reset Workaround -----------------------------------------------------------------------

class PCIeLinkResetWorkaround(LiteXModule):
    """
    Periodically toggles PCIe reset until link-up is observed.

    Behavior:
    - If link is down on a timer tick, assert reset low.
    - On next timer tick, release reset high.
    - Once link is up, keep reset released.
    """
    def __init__(self, link_up, sys_clk_freq, interval_s=10e-3, interval_cycles=None):
        self.rst_n = Signal(reset=1)

        # # #

        # Synchronize link status into sys domain.
        link_up_sys = Signal()
        self.specials += MultiReg(link_up, link_up_sys)

        # Retry timer.
        if interval_cycles is None:
            interval_cycles = max(1, int(interval_s * sys_clk_freq))
        self.timer = timer = WaitTimer(interval_cycles)
        self.comb += timer.wait.eq(~timer.done)

        # Reset sequencing:
        # - If currently in reset, release it.
        # - Else, if link is still down, assert reset.
        self.sync += If(timer.done,
            If(~self.rst_n,
                self.rst_n.eq(1)
            ).Elif(~link_up_sys,
                self.rst_n.eq(0)
            )
        )
