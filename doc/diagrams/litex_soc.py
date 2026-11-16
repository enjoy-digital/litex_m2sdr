#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# A LiteX SoC without CPU: host bridges are the masters of the bus that reaches the registers.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 270)
    d.add(Container(120, 10, 630, 250, title="FPGA", title_pos="br"))
    host = d.add(Box(10, 105, 75, 50, label="Host", fill=COLOR_HARDWARE))
    bus  = d.add(Box(310, 30, 90, 200, label="SoC bus", fill=COLOR_ACCENT))
    d.add(Bus([(85, 130), (102, 130)]))
    d.add(Bus([(102, 50), (102, 210)]))
    bridges = ["PCIe bridge", "Etherbone", "JTAGBone"]
    cores   = ["RFIC interface", "Board time", "Clock generator"]
    for i, (bridge, core) in enumerate(zip(bridges, cores)):
        y = 30 + 80*i
        b = d.add(Box(140, y, 120, 40, label=bridge, fill=COLOR_ACCENT_LIGHT))
        c = d.add(Box(520, y, 140, 40, label=core))
        d.add(Arrow((102, y + 20), (b, "l"), heads="both"))
        d.add(Arrow((b, "r"), (310, y + 20)))
        d.add(Arrow((400, y + 20), (c, "l"), heads="both", label="registers"))
    return d
