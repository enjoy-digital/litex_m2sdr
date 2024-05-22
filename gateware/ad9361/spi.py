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
                ("``0b0``", "No Action."),
                ("``0b1``", "Start Xfer."),
            ], description="SPI Xfer Start"),
            CSRField("length", size=8, offset=8, values=[
                ("``  8``", " 8-bit SPI Xfer."),
                ("`` 16``", "16-bit SPI Xfer."),
                ("`` 24``", "24-bit SPI Xfer."),
            ], description="SPI Xfer Length (in bits).")
        ])
        self._status  = CSRStatus(fields=[
            CSRField("done", size=1, offset=0, values=[
                ("``0b0``", "SPI Xfer Onggoing."),
                ("``0b1``", "SPI Xfer Done."),
            ], description="SPI Xfer Done."),
        ])
        self._mosi = CSRStorage(width)
        self._miso = CSRStatus(width)

        self.irq = Signal()

        # # #

        # Signals.
        # --------
        start  = self._control.fields.start
        length = self._control.fields.length
        cs     = Signal()
        shift  = Signal()
        done   = self._status.fields.done

        # Clk Div/Gen.
        # ------------
        i       = Signal(max=div)
        clk_en  = Signal()
        set_clk = Signal()
        clr_clk = Signal()
        self.sync += [
            If(set_clk,
                pads.clk.eq(cs)
            ),
            If(clr_clk,
                pads.clk.eq(0),
                i.eq(0)
            ).Else(
                i.eq(i + 1),
            )
        ]

        self.comb +=[
            set_clk.eq(i==div//2-1),
            clr_clk.eq(i==div-1)
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
            If(clr_clk,
                NextState("SHIFT")
            ),
        )
        fsm.act("SHIFT",
            If(cnt == length,
                NextState("END")
            ).Else(
                NextValue(cnt, cnt + clr_clk)
            ),
            cs.eq(1),
            shift.eq(1),
        )
        fsm.act("END",
            If(set_clk,
                NextState("IDLE")
            ),
            shift.eq(1),
            self.irq.eq(1)
        )

        # Chip Select.
        # ------------
        self.comb += pads.cs_n.eq(~cs)

        # MOSI.
        # -----
        mosi    = Signal()
        sr_mosi = Signal(width)

        # Propagate on Clk Rising Edge (CPHA=1)
        self.sync += [
            If(start,
                sr_mosi.eq(self._mosi.storage)
            ).Elif(clr_clk & shift,
                sr_mosi.eq(Cat(Signal(), sr_mosi[:-1]))
            ).Elif(set_clk,
                pads.mosi.eq(sr_mosi[-1])
            )
        ]

        # MISO.
        # -----
        miso    = Signal()
        sr_miso = Signal(width)

        # Capture on Clk Falling Edge (CPHA=1).
        self.sync += [
            If(shift,
                If(clr_clk,
                    miso.eq(pads.miso),
                ).Elif(set_clk,
                    sr_miso.eq(Cat(miso, sr_miso[:-1]))
                )
            )
        ]
        self.comb += self._miso.status.eq(sr_miso)
