#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer

# Constants / Helpers ------------------------------------------------------------------------------

def _prbs16_next(state):
    """One step of the AD9361 BIST PRBS LFSR (taps 1,2,4..15; LSB-in shift)."""
    return Cat((
        state[1]  ^ state[2]  ^ state[4]  ^ state[5]  ^
        state[6]  ^ state[7]  ^ state[8]  ^ state[9]  ^
        state[10] ^ state[11] ^ state[12] ^ state[13] ^
        state[14] ^ state[15]),
        state[:-1]
    )

# AD9361 PRBS Generator ----------------------------------------------------------------------------

class AD9361PRBSGenerator(LiteXModule):
    def __init__(self, seed=0x0a54):
        self.ce = Signal(reset=1)
        self.o  = Signal(16)

        # # #

        # PRBS Generation.
        data = Signal(16, reset=seed)
        self.sync += If(self.ce, data.eq(_prbs16_next(data)))
        self.comb += self.o.eq(data)

# AD9361 PRBS 1R1T Generator -----------------------------------------------------------------------

class AD9361PRBS1R1TGenerator(LiteXModule):
    """PRBS generator for 1R1T mode.

    In 1R1T mode the a/b slots represent two consecutive samples of the same
    stream. Export both samples for the current word and advance the reference
    by two words on each accepted PHY word.
    """
    def __init__(self, seed=0x0a54):
        self.ce     = Signal(reset=1)
        self.o      = Signal(16)
        self.o_next = Signal(16)

        # # #

        data   = Signal(16, reset=seed)
        data_n = Signal(16)

        self.comb += data_n.eq(_prbs16_next(data))
        self.sync += If(self.ce, data.eq(_prbs16_next(data_n)))
        self.comb += [
            self.o.eq(data),
            self.o_next.eq(data_n),
        ]

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

# AD9361 PRBS 1R1T Checker -------------------------------------------------------------------------

class AD9361PRBS1R1TChecker(LiteXModule):
    """PRBS checker for 1R1T mode.

    In 1R1T mode the PHY packs two CONSECUTIVE samples of one stream into the
    a/b slots of each word, so each lane sees every other PRBS value and the
    per-lane checkers (which expect consecutive values) can never sync. This
    checker validates the interleaved pair: ia must match the reference and ib
    the reference advanced by one; the reference advances by two per word.
    Since the LFSR period (2^16-1) is odd, the stride-2 subsequence still
    visits every state, so seed-based resynchronization works unchanged.
    """
    def __init__(self, seed=0x0a54):
        self.ce     = Signal(reset=1)
        self.ia     = Signal(12)
        self.ib     = Signal(12)
        self.synced = Signal()

        # # #

        error   = Signal()
        state   = Signal(16, reset=seed)
        state_n = Signal(16)

        # PRBS reference (advance by 2 per word, reseed on error).
        self.comb += state_n.eq(_prbs16_next(state))
        self.sync += If(self.ce,
            If(error,
                state.eq(seed)
            ).Else(
                state.eq(_prbs16_next(state_n))
            )
        )

        # Error generation.
        self.comb += If(self.ce,
            error.eq((self.ia != state[:12]) | (self.ib != state_n[:12]))
        )

        # Sync generation.
        self.sync_timer = WaitTimer(1024)
        self.comb += self.sync_timer.wait.eq(~error)
        self.comb += self.synced.eq(self.sync_timer.done)
