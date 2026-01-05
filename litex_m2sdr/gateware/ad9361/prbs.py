#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer

# AD9361 PRBS Generator ----------------------------------------------------------------------------

class AD9361PRBSGenerator(LiteXModule):
    def __init__(self, seed=0x0a54):
        self.ce = Signal(reset=1)
        self.o  = Signal(16)

        # # #

        # PRBS Generation.
        data = Signal(16, reset=seed)
        self.sync += If(self.ce, data.eq(Cat((
            data[1]  ^ data[2]  ^ data[4]  ^ data[5]  ^
            data[6]  ^ data[7]  ^ data[8]  ^ data[9]  ^
            data[10] ^ data[11] ^ data[12] ^ data[13] ^
            data[14] ^ data[15]),
            data[:-1]
            )
        ))
        self.comb += self.o.eq(data)

# AD9361 PRBS Checker ----------------------------------------------------------------------------

class AD9361PRBSChecker(LiteXModule):
    def __init__(self, seed=0x0a54):
        self.ce     = Signal(reset=1)
        self.i      = Signal(12)
        self.synced = Signal()

        # # #

        error = Signal()

        # # #

        # PRBS reference.
        prbs = AD9361PRBSGenerator(seed=seed)
        prbs = ResetInserter()(prbs)
        self.submodules += prbs

        # PRBS re-synchronization.
        self.comb += prbs.ce.eq(self.ce)
        self.comb += prbs.reset.eq(error)

        # Error generation.
        self.comb += If(self.ce, error.eq(self.i != prbs.o[:12]))


        # Sync generation.
        self.sync_timer = WaitTimer(1024)
        self.comb += self.sync_timer.wait.eq(~error)
        self.comb += self.synced.eq(self.sync_timer.done)
