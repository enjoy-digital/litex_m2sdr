#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Building the gateware: from the build options to a loaded or flashed board.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 270)
    opts = d.add(Box( 15,  40, 105, 60, label="Build\noptions"))
    soc  = d.add(Box(145,  40, 105, 60, label="litex_m2sdr.py", fill=COLOR_ACCENT))
    viv  = d.add(Box(275,  40,  95, 60, label="Vivado"))
    bit  = d.add(Box(395,  40, 105, 60, label="Bitstream\nfiles", fill=COLOR_ACCENT_LIGHT))
    ofl  = d.add(Box(525,  40, 105, 60, label="JTAG\nprogrammer"))
    brd  = d.add(Box(660,  40,  85, 190, label="Board", fill=COLOR_HARDWARE))
    csr  = d.add(Box(145, 170, 105, 60, label="Register\nexports", fill=COLOR_ACCENT_LIGHT))
    sw   = d.add(Box(275, 170,  95, 60, label="Host\nsoftware"))
    fl   = d.add(Box(525, 170, 105, 60, label="Flash over\nPCIe"))
    d.add(Arrow((opts, "r"), (soc, "l"), kind="control"))
    d.add(Arrow((soc, "r"), (viv, "l"), kind="data"))
    d.add(Arrow((viv, "r"), (bit, "l"), kind="data"))
    d.add(Arrow((bit, "r"), (ofl, "l"), kind="data"))
    d.add(Arrow((ofl, "r"), (660, 70), kind="accent", label="load, flash", label_pos=(645, 28)))
    d.add(Arrow((soc, "b"), (csr, "t"), kind="data"))
    d.add(Arrow((csr, "r"), (sw, "l"), kind="data"))
    d.add(Arrow((bit, "b"), (fl, "l"), kind="data"))
    d.add(Arrow((fl, "r"), (660, 200), kind="accent", label="flash", label_pos=(645, 158)))
    return d
