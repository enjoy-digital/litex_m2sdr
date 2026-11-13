#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Use of the four transceiver lanes of the M.2 connector in each variant.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 290)
    d.add(Text(125, 35, "variant m2", size=FONT_SIZE, italic=True))
    d.add(Text(380, 35, "FPGA transceivers", size=FONT_SIZE, italic=True))
    d.add(Text(640, 35, "variant baseboard", size=FONT_SIZE, italic=True))
    host  = d.add(Box(40, 60, 170, 200, label="M.2 slot of a host\nPCIe x1, x2 or x4", fill=COLOR_HARDWARE))
    lanes = [d.add(Box(330, 60 + 50*i, 100, 50, label=f"Lane {i}", radius=0, fill=COLOR_ACCENT)) for i in range(4)]
    names = ["SATA connector", "SFP1 cage", "SFP0 cage", "PCIe x1 edge"]
    for i, lane in enumerate(lanes):
        dst = d.add(Box(560, 65 + 50*i, 160, 40, label=names[i], fill=COLOR_HARDWARE))
        d.add(Arrow((210, 85 + 50*i), (lane, "l"), kind="data", heads="both"))
        d.add(Arrow((lane, "r"), (dst, "l"), kind="data", heads="both"))
    return d
