#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex.gen import *

from litex.soc.interconnect.csr import *

from functools import reduce
from operator import or_

# Constants ----------------------------------------------------------------------------------------

AGC_DEFAULT_LOW_THRESHOLD  = 1536 # -2.5 dBFS warning threshold for 12-bit RFIC samples.
AGC_DEFAULT_HIGH_THRESHOLD = 2016 # Near-clip threshold for 12-bit RFIC samples.

# Helpers ------------------------------------------------------------------------------------------

def signed_abs(module, value):
    signed    = Signal((len(value) + 1, True))
    magnitude = Signal(len(value))
    module.comb += [
        signed.eq(Cat(value, value[-1])),
        magnitude.eq(Mux(signed < 0, -signed, signed)),
    ]
    return magnitude

# AGC Saturation Count -----------------------------------------------------------------------------

class AGCSaturationCount(LiteXModule):
    def __init__(self, ce, iqs, inc=1, threshold_reset=AGC_DEFAULT_HIGH_THRESHOLD):
        self.control = CSRStorage(fields=[
            CSRField("enable", offset=0, size=1, values=[
                ("``0b0``", "Disable AGC Saturation Count."),
                ("``0b1``", "Enable  AGC Saturation Count.")
            ]),
            CSRField("clear",     offset=1,   size=1, pulse=True, description="Clear Saturation Count."),
            CSRField("threshold", offset=16, size=16, reset=threshold_reset,
                description="Saturation Threshold (ABS, in RFIC 12-bit sample units)."),
        ])
        self.status = CSRStatus(fields=[
            CSRField("count", size=32, description="Saturation event count (one event per accepted beat).")
        ])

        # # #

        # Map CSRs to/from Signals.
        enable    = self.control.fields.enable
        clear     = self.control.fields.clear
        threshold = self.control.fields.threshold
        count     = self.status.fields.count

        # Compute ABS of I/Q and collapse all observed lanes into one event.
        iqs_abs   = [signed_abs(self, iq) for iq in iqs]
        saturated = Signal()
        self.comb += saturated.eq(reduce(or_, [iq_abs >= threshold for iq_abs in iqs_abs]))

        # Compute Saturation.
        self.sync += [
            # Clear Count when Disable or Clear.
            If(~enable | clear,
                # Clear Count.
                count.eq(0),
            # Else check for Saturation.
            ).Elif(ce & saturated & (count <= (2**len(count) - 1 - inc)),
                count.eq(count + inc),
            )
        ]
