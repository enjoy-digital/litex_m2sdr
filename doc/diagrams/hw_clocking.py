#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Clocking tree of the board with the SI5351C: oscillator, FPGA clocking, 10 MHz selection, clock
# generator, board time and RFIC.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 350)
    d.add(Container(170, 20, 400, 220, title="FPGA", title_pos="br"))
    xo   = d.add(Box( 20,  50, 110, 50, label="100 MHz XO", fill=COLOR_HARDWARE))
    ext  = d.add(Box( 20, 150, 110, 50, label="10 MHz input", fill=COLOR_HARDWARE))
    pll  = d.add(Box(200,  50, 100, 50, label="PLL, MMCM"))
    mux  = d.add(Box(200, 150, 100, 50, label="10 MHz mux"))
    phy  = d.add(Box(410,  50, 130, 50, label="RFIC interface"))
    time = d.add(Box(410, 150, 130, 50, label="Board time"))
    si   = d.add(Box(200, 280, 160, 50, label="SI5351C", fill=COLOR_ACCENT))
    rfic = d.add(Box(600, 280, 140, 50, label="RFIC AD9361", fill=COLOR_ACCENT_LIGHT))
    d.add(Arrow((xo, "r"), (pll, "l"), kind="control"))
    d.add(Arrow((ext, "r"), (mux, "l"), kind="control"))
    d.add(Arrow((pll, "b"), (mux, "t"), kind="control", label="10 MHz", label_side="right"))
    d.add(Arrow((mux, "b"), (250, 280), kind="control", label="CLKIN", label_pos=(275, 262)))
    d.add(Arrow((340, 280), (time, "l"), via=[(340, 175)], kind="control", label="100 MHz",
        label_pos=(372, 262)))
    d.add(Arrow((si, "r"), (rfic, "l"), kind="accent", label="reference"))
    d.add(Arrow((670, 280), (phy, "r"), via=[(670, 75)], kind="control", label="DATA_CLK",
        label_pos=(705, 180)))
    return d
