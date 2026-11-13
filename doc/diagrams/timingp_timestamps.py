#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Timestamps: RX buffers are stamped with the board time, a timed TX buffer waits for its time.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 200)
    x0, w = 120, 180
    d.add(Arrow((60, 160), (730, 160), label="board time", label_pos=(700, 177)))
    for i in range(3):
        x = x0 + w*i
        d.add(Box(x, 25, w, 36, label="RX buffer", radius=0, fill=COLOR_ACCENT_LIGHT))
        d.add(Bus([(x, 61), (x, 160)], dash="2,3"))
        d.add(Text(x, 177, f"t{i}", size=12))
    d.add(Box(x0 + 2*w, 95, w, 36, label="TX buffer", radius=0, fill=COLOR_ACCENT))
    d.add(Text(x0 + w + w/2, 113, "held until t2", size=12))
    d.add(Text(60, 43, "RX", size=15))
    d.add(Text(60, 113, "TX", size=15))
    return d
