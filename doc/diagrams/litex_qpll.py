#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Shared PLLs of a GTP quad: two PLLs, four transceivers, each transceiver uses one PLL.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 270)
    d.add(Container(150, 10, 600, 250, title="GTP quad", title_pos="bl"))
    plls = []
    for i, y in enumerate((45, 175)):
        pll = d.add(Box(175, y, 110, 40, label=f"PLL{i}", fill=COLOR_ACCENT))
        d.add(Arrow((20, y + 20), (pll, "l"), label=f"reference {i}"))
        plls.append(pll)
    d.add(Bus([(285, 65), (380, 65)]))
    d.add(Bus([(380, 35), (380, 215)]))
    d.add(Bus([(285, 195), (420, 195)]))
    d.add(Bus([(420, 51), (420, 231)]))
    for i in range(4):
        y = 22 + 60*i
        gt = d.add(Box(520, y, 150, 44, label=f"Transceiver {i}", fill=COLOR_HARDWARE))
        d.add(Arrow((380, y + 13), (520, y + 13)))
        d.add(Arrow((420, y + 31), (520, y + 31)))
        d.add(Arrow((670, y + 22), (735, y + 22), kind="data", heads="both"))
    return d
