#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream

# Value/Strobe CDC ---------------------------------------------------------------------------------

class ValueStrobeCDC(LiteXModule):
    """Cross value(s) together with their strobe between clock domains.

    The payload travels with the strobe through a single handshaked crossing,
    so the destination never samples a half-settled word -- unlike per-bit
    MultiReg synchronizers, which have no bus coherence.

    Inputs live in cd_from, outputs in cd_to:
    - strobe / strobe_o : transfer request and resulting 1-cycle pulse. With
      on_change=True the strobe input is ignored and a transfer is initiated
      whenever one of the input values changes.
    - One <name> input and <name>_o output Signal per layout field (layout
      may also be an int, shorthand for [("value", width)]). Outputs are
      combinational from the crossing and only meaningful while strobe_o is
      asserted; callers needing a held value register it in cd_to on
      strobe_o.
    """
    def __init__(self, layout, cd_from="sys", cd_to="sys", on_change=False):
        if isinstance(layout, int):
            layout = [("value", layout)]
        self.strobe   = Signal()
        self.strobe_o = Signal()
        for name, width in layout:
            setattr(self, name,        Signal(width))
            setattr(self, name + "_o", Signal(width))

        # # #

        self.cdc = cdc = stream.ClockDomainCrossing(layout, cd_from=cd_from, cd_to=cd_to)

        strobe = self.strobe
        if on_change:
            values      = Cat(*[getattr(self, name) for name, _ in layout])
            values_prev = Signal(len(values))
            sync_from   = getattr(self.sync, cd_from)
            sync_from  += values_prev.eq(values)
            strobe      = values != values_prev

        self.comb += [
            cdc.sink.valid.eq(strobe),
            cdc.source.ready.eq(1),
            self.strobe_o.eq(cdc.source.valid),
        ]
        for name, _ in layout:
            self.comb += [
                getattr(cdc.sink, name).eq(getattr(self, name)),
                getattr(self, name + "_o").eq(getattr(cdc.source, name)),
            ]
