#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex.gen import *

from litex.soc.interconnect.csr import *

# Helpers ------------------------------------------------------------------------------------------

def twos_complement(sign, value, dest):
    return [
        dest.eq(value),
        If(sign,
            dest.eq(~value + 1)
        )
    ]

# AGC Saturation Count -----------------------------------------------------------------------------

class AGCSaturationCount(LiteXModule):
    def __init__(self, ce, iqs, inc=1):
        self.control = CSRStorage(fields=[
            CSRField("enable", offset=0, size=1, values=[
                ("``0b0``", "Disable AGC Saturation Count."),
                ("``0b1``", "Enable  AGC Saturation Count.")
            ]),
            CSRField("clear",     offset=1,   size=1, pulse=True, description="Clear Saturation Count."),
            CSRField("threshold", offset=16, size=16,             description="Saturation Threshold (ABS)."),
        ])
        self.status = CSRStatus(fields=[
            CSRField("count", size=32, description="Saturation Count (in Samples).")
        ])

        # # #

        # Map CSRs to/from Signals.
        enable    = self.control.fields.enable
        clear     = self.control.fields.clear
        threshold = self.control.fields.threshold
        count     = self.status.fields.count

        # Compute ABS of I/Q.
        iqs_abs = [Signal(len(d) - 1) for d in iqs]
        for iq, iq_abs in zip(iqs, iqs_abs):
            self.comb += twos_complement(sign=iq[-1], value=iq[:-1], dest=iq_abs)

        # Compute Saturation.
        self.sync += [
            # Clear Count when Disable or Clear.
            If(~enable | clear,
                # Clear Count.
                count.eq(0),
            # Else check for Saturation.
            ).Elif(ce & (count != (2**len(count) - inc)),
                # Increment Count when Abs(I) or Abs(Q) >= Threshold.
                *[If(iq_abs >= threshold, count.eq(count + inc)) for iq_abs in iqs_abs]
            )
        ]
