#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Debug method on the board: from the cheapest check to the most expensive one.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 130)
    labels = ["Board\nidentity", "Registers", "Loopback\ntests", "LiteScope\ncapture", "Verilog\nand timing"]
    fills  = [COLOR_ACCENT, COLOR_ACCENT, COLOR_ACCENT_LIGHT, COLOR_ACCENT_LIGHT, COLOR_FILL]
    boxes  = [d.add(Box(15 + i*150, 35, 125, 60, label=l, fill=f)) for i, (l, f) in enumerate(zip(labels, fills))]
    for a, b in zip(boxes, boxes[1:]):
        d.add(Arrow((a, "r"), (b, "l"), kind="data"))
    return d
