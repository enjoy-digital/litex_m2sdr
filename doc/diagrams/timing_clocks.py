#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Reference clock selection of the SI5351 and the clocks it gives to the FPGA and the RFIC.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    ufl  = d.add(Box(20,  40, 130, 60, label="uFL connector\n10 MHz", fill=COLOR_HARDWARE))
    mmcm = d.add(Box(20, 135, 130, 60, label="clk10 MMCM\n10 MHz", fill=COLOR_ACCENT_LIGHT))
    mux  = d.add(Box(205, 70, 110, 95, label="Clock mux\nclkin_src", fill=COLOR_ACCENT))
    si   = d.add(Box(380, 40, 130, 250, label="SI5351\nB or C", fill=COLOR_HARDWARE))
    ref  = d.add(Box(600,  40, 140, 60, label="AD9361\nreference", fill=COLOR_HARDWARE))
    tim  = d.add(Box(600, 135, 140, 60, label="TimeGenerator\n100 MHz"))
    i2c  = d.add(Box(205, 230, 110, 60, label="I2C master\nsequencer", fill=COLOR_ACCENT))
    d.add(Arrow((ufl,  "r"), (mux, "l", 0.3), kind="control"))
    d.add(Arrow((mmcm, "r"), (mux, "l", 0.7), kind="control"))
    d.add(Arrow((mux, "r"), (380, 117.5), kind="accent", label="CLKIN"))
    d.add(Arrow((i2c, "r"), (380, 260), kind="control", heads="both", label="config"))
    d.add(Arrow((510,  70), (ref, "l"), kind="data"))
    d.add(Arrow((510, 165), (tim, "l"), kind="data", label="si5351_clk1"))
    return d
