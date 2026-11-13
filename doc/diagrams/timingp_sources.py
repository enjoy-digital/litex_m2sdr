#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Four ways to bring a time and frequency reference to the board.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 260)
    brd = d.add(Box(560, 20, 180, 220, label="Board time,\nreference clock", fill=COLOR_ACCENT))
    rows = [
        ("10 MHz reference", "coaxial cable"),
        ("PCIe PTM",       "PCIe link"),
        ("IEEE 1588 PTP",  "Ethernet"),
        ("White Rabbit",   "optical fibre"),
    ]
    for i, (name, medium) in enumerate(rows):
        y = 20 + 60*i
        b = d.add(Box(250, y, 170, 40, label=name, fill=COLOR_ACCENT_LIGHT))
        d.add(Arrow((30, y + 20), (b, "l"), kind="data", label=medium))
        d.add(Arrow((b, "r"), (560, y + 20)))
    return d
