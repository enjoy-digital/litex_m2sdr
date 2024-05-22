#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *

# SPIMaster ----------------------------------------------------------------------------------------

class SPIMaster(LiteXModule):
    def __init__(self, pads, width=24, div=2):
        self.pads = pads

        self._control = CSRStorage(fields=[
            CSRField("start",  size=1, offset=0, pulse=True, values=[
                ("``0b0``", "No action."),
                ("``0b1``", "Start transfer."),
            ], description="Start SPI transfer."),
            CSRField("length", size=8, offset=8, values=[
                ("``  8``", "8-bit transfer."),
                ("`` 16``", "16-bit transfer."),
                ("`` 24``", "24-bit transfer."),
            ], description="Transfer length in bits.")
        ])
        self._status  = CSRStatus(fields=[
            CSRField("done", size=1, offset=0, values=[
                ("``0b0``", "Transfer ongoing."),
                ("``0b1``", "Transfer done."),
            ], description="Transfer status."),
        ])
        self._mosi = CSRStorage(width)
        self._miso = CSRStatus(width)

        # # #

        # Signals.
        # --------
        start       = self._control.fields.start
        length      = self._control.fields.length
        done        = self._status.fields.done
        chip_select = Signal()
        shift       = Signal()

        # Clk Div/Gen.
        # ------------
        clk_count = Signal(max=div)
        clk_set   = Signal()
        clk_clr   = Signal()
        self.sync += [
            clk_count.eq(clk_count + 1),
            If(clk_set,
                pads.clk.eq(chip_select)
            ),
            If(clk_clr,
                pads.clk.eq(0),
                clk_count.eq(0)
            ),
        ]
        self.comb +=[
            clk_set.eq(clk_count ==  ((div//2) - 1)),
            clk_clr.eq(clk_count ==       (div - 1)),
        ]

        # FSM.
        # ----
        cnt     = Signal(8)
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(start,
                NextState("WAIT_CLK")
            ),
            done.eq(1),
            NextValue(cnt, 0),
        )
        fsm.act("WAIT_CLK",
            If(clk_clr,
                NextState("SHIFT")
            ),
        )
        fsm.act("SHIFT",
            If(cnt == length,
                NextState("END")
            ).Else(
                NextValue(cnt, cnt + clk_clr)
            ),
            chip_select.eq(1),
            shift.eq(1),
        )
        fsm.act("END",
            If(clk_set,
                NextState("IDLE")
            ),
            shift.eq(1),
        )

        # Chip Select Output.
        # -------------------
        self.comb += pads.cs_n.eq(~chip_select)

        # MOSI Shift/Output.
        # ------------------
        # Propagate on Clk Rising Edge (CPHA=1).
        mosi           = Signal()
        mosi_shift_reg = Signal(width)
        self.sync += [
            If(start,
                mosi_shift_reg.eq(self._mosi.storage)
            ).Elif(clk_clr & shift,
                mosi_shift_reg.eq(Cat(Signal(), mosi_shift_reg[:-1]))
            ).Elif(clk_set,
                pads.mosi.eq(mosi_shift_reg[-1])
            )
        ]

        # MISO Input/Shift.
        # -----------------
        # Capture on Clk Falling Edge (CPHA=1).
        miso           = Signal()
        miso_shift_reg = Signal(width)
        self.sync += [
            If(shift,
                If(clk_clr,
                    miso.eq(pads.miso),
                ).Elif(clk_set,
                    miso_shift_reg.eq(Cat(miso, miso_shift_reg[:-1]))
                )
            )
        ]
        self.comb += self._miso.status.eq(miso_shift_reg)
