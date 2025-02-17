#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *

from litex.gen.genlib.misc import WaitTimer

# PPS Generator ------------------------------------------------------------------------------------

class PPSGenerator(LiteXModule):
    def __init__(self, clk_freq, time, offset=int(500e6), reset=0):
        self.pps    = Signal() # PPS Output.

        # # #

        # PPS Signals.
        start = Signal()
        count = Signal(32)

        # PPS FSM.
        self.fsm = fsm = ResetInserter()(FSM(reset_state="IDLE"))
        self.comb += fsm.reset.eq(reset)
        fsm.act("IDLE",
            NextValue(count, 0),
            If(time != 0,
                NextState("RUN")
            )
        )
        fsm.act("RUN",
            If(time > ((count * int(1e9) + offset)),
                start.eq(1),
                NextValue(count, count + 1)
            )
        )

        # PPS Generation.
        self.timer = WaitTimer(clk_freq*200e-3) # 20% High / 80% Low PPS.
        self.comb += self.timer.wait.eq(~start)
        self.comb += self.pps.eq(~self.timer.done)
