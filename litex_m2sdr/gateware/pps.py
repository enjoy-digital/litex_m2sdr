#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *

from litex.gen.genlib.misc import WaitTimer

# PPS Input ---------------------------------------------------------------------------------------

class PPSInput(LiteXModule):
    """Synchronize and monitor a 3.3V external PPS input in the sys domain."""
    def __init__(self, pad):
        self._status = CSRStatus(fields=[
            CSRField("level", size=1, description="Synchronized PPS input level."),
            CSRField("pulse", size=1, description="One-cycle pulse on PPS rising edge."),
        ])
        self._count  = CSRStatus(32, description="Number of detected PPS rising edges.")
        self.pulse   = Signal()
        self.level   = Signal()
        self.count   = Signal(32)

        # # #

        # Synchronize the asynchronous M.2 input before edge detection.
        self.specials += MultiReg(pad, self.level)
        previous = Signal()
        self.sync += [
            previous.eq(self.level),
            self.pulse.eq(self.level & ~previous),
            If(self.level & ~previous,
                self.count.eq(self.count + 1)
            ),
        ]
        self.comb += [
            self._status.fields.level.eq(self.level),
            self._status.fields.pulse.eq(self.pulse),
            self._count.status.eq(self.count),
        ]

# PPS Generator ------------------------------------------------------------------------------------

class PPSGenerator(LiteXModule):
    """
    PPS Generator Module.

    Generates a Pulse Per Second (PPS) signal with a 10% high / 90% low duty cycle based on a
    specified time input and clock frequency. Includes an offset to adjust the timing.
    """
    def __init__(self, clk_freq, time, offset=int(500e6), reset=0):
        # IOs.
        self.pps       = pps       = Signal() # PPS output.
        self.pps_pulse = pps_pulse = Signal() # PPS pulse output.
        self.count     = Signal(32)          # PPS pulse count / coarse seconds counter.

        # # #

        # Parameters.
        # -----------
        self.clk_freq = clk_freq  # System clock frequency in Hz.
        self.offset   = offset    # Timing offset in ns.

        # Signals.
        # --------
        self.start     = start     = Signal()   # Trigger for PPS pulse.
        self.time_next = time_next = Signal(64) # Next Time Value for PPS pulse.

        # FSM.
        # ----
        self.fsm = fsm = ResetInserter()(FSM(reset_state="IDLE"))
        self.comb += fsm.reset.eq(reset)

        # Idle State: Wait for valid time input.
        fsm.act("IDLE",
            NextValue(time_next, int(1e9) + offset),
            If(time != 0,
                NextState("RUN")
            )
        )

        # Run State: Count and trigger PPS.
        fsm.act("RUN",
            If(time > time_next,
                start.eq(1),
                NextValue(time_next, time_next + int(1e9))
            )
        )

        # PPS Pulse Generation.
        # --------------------
        self.timer = WaitTimer(int(clk_freq * 100e-3))  # 10% high pulse width.
        self.comb += [
            self.timer.wait.eq(~start),
            pps.eq(~self.timer.done),
            pps_pulse.eq(start),
        ]
        self.sync += [
            If(reset,
                self.count.eq(0)
            ).Elif(start,
                self.count.eq(self.count + 1)
            )
        ]
