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

def _prbs16_next_int(state):
    """Python-side constant evaluation of _prbs16_next (for reseed constants)."""
    fb = 0
    for b in (1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15):
        fb ^= (state >> b) & 1
    return ((state << 1) | fb) & 0xFFFF

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

        # Input registration: the input data comes from the PHY RX mux; registering it (and ce)
        # keeps that mux cone off the compare path at high DATA_CLK rates (491.52MHz with
        # Oversampling). The compare and re-seed stay a single-cycle recurrence on registered
        # operands - splitting them across cycles would break the per-sample re-seed alignment
        # the self-synchronization relies on.
        i_r  = Signal(12)
        ce_r = Signal()
        self.sync += [
            ce_r.eq(self.ce),
            If(self.ce, i_r.eq(self.i)),
        ]

        # PRBS reference with seed-restart re-synchronization: on each accepted sample the
        # reference either advances (match) or restarts from the seed (mismatch), so it locks
        # once the incoming sequence passes the seed value. The capture is retimed one cycle
        # after the input registration (ce_rr): both compare operands are then stable for two
        # cycles before every capture by construction, which a matching multicycle constraint
        # uses to close the compare/re-seed recurrence at 491.52MHz DATA_CLK (the per-sample
        # recurrence semantics are unchanged for any ce cadence).
        ce_rr = Signal()
        self.sync += ce_rr.eq(ce_r)
        state = Signal(16, reset=seed)
        error = Signal()
        self.comb += error.eq(ce_rr & (i_r != state[:12]))
        self.sync += If(ce_rr, state.eq(Mux(error, seed, _prbs16_next(state))))

        # Sync generation. The timer counts on a registered error copy: its reset fans out to
        # the whole counter and would otherwise put that fanout on the compare cone. synced
        # still drops combinationally with the error itself (a single-gate load on the compare).
        error_r = Signal()
        self.sync += error_r.eq(error)
        self.sync_timer = WaitTimer(1024)
        self.comb += self.sync_timer.wait.eq(~error_r)
        self.comb += self.synced.eq(self.sync_timer.done & ~error)

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

        # Input registration: keeps the PHY RX mux cone off the compare path (the checker is
        # instantiated in the rfic domain, constrained at 491.52MHz on Oversampling builds even
        # though 1R1T itself runs DATA_CLK at no more than 245.76MHz).
        ia_r = Signal(12)
        ib_r = Signal(12)
        ce_r = Signal()
        self.sync += [
            ce_r.eq(self.ce),
            If(self.ce,
                ia_r.eq(self.ia),
                ib_r.eq(self.ib),
            )
        ]
        # Capture retiming (see AD9361PRBSChecker): both compare operands stable two cycles
        # before every capture by construction, closed with a matching multicycle constraint.
        ce_rr = Signal()
        self.sync += ce_rr.eq(ce_r)

        error   = Signal()
        # PRBS reference (advances by 2 per word, reseeds on error). The compare and re-seed
        # stay a single-cycle recurrence - splitting them across cycles would break the
        # per-sample re-seed alignment the self-synchronization relies on. To keep the cycle
        # shallow at 491.52MHz, the reference is held as a register PAIR (state, state_n) with
        # the invariant state_n == next(state): both slot compares then read registers directly
        # and the LFSR-next evaluations sit on the parallel data legs, not on the compare cone.
        state   = Signal(16, reset=seed)
        state_n = Signal(16, reset=_prbs16_next_int(seed))
        self.sync += If(ce_rr,
            state.eq(  Mux(error, seed,                  _prbs16_next(state_n))),
            state_n.eq(Mux(error, _prbs16_next_int(seed), _prbs16_next(_prbs16_next(state_n)))),
        )

        # Error generation.
        self.comb += If(ce_rr,
            error.eq((ia_r != state[:12]) | (ib_r != state_n[:12]))
        )

        # Sync generation. The timer counts on a registered error copy: its reset fans out to
        # the whole counter and would otherwise put that fanout on the compare cone. synced
        # still drops combinationally with the error itself (a single-gate load on the compare).
        error_r = Signal()
        self.sync += error_r.eq(error)
        self.sync_timer = WaitTimer(1024)
        self.comb += self.sync_timer.wait.eq(~error_r)
        self.comb += self.synced.eq(self.sync_timer.done & ~error)
