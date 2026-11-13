#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# LVDS data port of the AD9361: a clock, a frame signal and six data pairs per direction.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 290)
    rfic = d.add(Box(20, 20, 150, 250, label="AD9361", fill=COLOR_HARDWARE))
    fpga = d.add(Box(590, 20, 150, 250, label="FPGA", fill=COLOR_ACCENT))
    signals = [
        ("DATA_CLK",   "control", True),
        ("RX_FRAME",   "control", True),
        ("RX_D[5:0]",  "data",    True),
        ("FB_CLK",     "control", False),
        ("TX_FRAME",   "control", False),
        ("TX_D[5:0]",  "data",    False),
    ]
    for i, (name, kind, to_fpga) in enumerate(signals):
        y = 55 + 38*i + (15 if i >= 3 else 0)
        a, b = ((170, y), (590, y)) if to_fpga else ((590, y), (170, y))
        d.add(Arrow(a, b, kind=kind, label=name))
    return d
